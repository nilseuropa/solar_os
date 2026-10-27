"""Differential LZ4 checks against the reference library, with output canaries."""
import ctypes
import ctypes.util
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CompressionTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        library = Path(cls.directory.name) / "dlp.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I" + str(ROOT / "protocol/display_link/include"),
                        str(ROOT / "protocol/display_link/src/sdlp.c"), "-o", str(library)], check=True)
        cls.dlp = ctypes.CDLL(str(library))
        cls.dlp.sdlp_lz4_encode.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                           ctypes.c_void_p, ctypes.c_size_t]
        cls.dlp.sdlp_lz4_encode.restype = ctypes.c_size_t
        cls.dlp.sdlp_lz4_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                           ctypes.c_void_p, ctypes.c_size_t]
        cls.dlp.sdlp_lz4_decode.restype = ctypes.c_bool
        cls.reference = ctypes.CDLL(ctypes.util.find_library("lz4") or "liblz4.so.1")
        cls.reference.LZ4_compress_default.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                                       ctypes.c_int, ctypes.c_int]
        cls.reference.LZ4_decompress_safe.argtypes = cls.reference.LZ4_compress_default.argtypes

    def decode(self, encoded, size):
        output = ctypes.create_string_buffer(b"\xa5" * (size + 32), size + 32)
        ok = self.dlp.sdlp_lz4_decode(ctypes.byref(output, 16), size, encoded, len(encoded))
        self.assertEqual(output.raw[:16], b"\xa5" * 16)
        self.assertEqual(output.raw[-16:], b"\xa5" * 16)
        return ok, output.raw[16:-16]

    def test_reference_interoperability_and_capacity(self):
        rng = random.Random(72613)
        for size in (*range(1, 100), 127, 128, 255, 256, 511, 512, 1000, 1024):
            for data in (bytes(size), (b"\xf8\0" * size)[:size],
                         bytes(k % 256 for k in range(size)), rng.randbytes(size)):
                reference = ctypes.create_string_buffer(size + 32)
                n = self.reference.LZ4_compress_default(data, reference, size, len(reference))
                self.assertGreater(n, 0)
                self.assertEqual(self.decode(reference.raw[:n], size), (True, data))
                for capacity in (1, 16, 206, size + 32):
                    output = ctypes.create_string_buffer(b"\xa5" * (capacity + 32), capacity + 32)
                    n = self.dlp.sdlp_lz4_encode(ctypes.byref(output, 16), capacity, data, size)
                    self.assertEqual(output.raw[:16], b"\xa5" * 16)
                    self.assertEqual(output.raw[-16:], b"\xa5" * 16)
                    self.assertLessEqual(n, capacity)
                    if n:
                        encoded = output.raw[16:16 + n]
                        self.assertEqual(self.decode(encoded, size), (True, data))
                        decoded = ctypes.create_string_buffer(size)
                        self.assertEqual(self.reference.LZ4_decompress_safe(encoded, decoded, n, size), size)
                        self.assertEqual(decoded.raw, data)
                    elif capacity == size + 32:
                        self.fail("Encoder failed with sufficient capacity")

    def test_malformed_blocks_stay_bounded(self):
        rng = random.Random(1423)
        for _ in range(20000):
            self.decode(rng.randbytes(rng.randrange(1, 207)), rng.randrange(1, 1025))
        for encoded in (b"\0\0\0", b"\0\1\0", b"\xff\xff", b"\x10"):
            self.assertFalse(self.decode(encoded, 1024)[0])
        self.assertFalse(self.decode(b"\x10\0", 1025)[0])


if __name__ == "__main__":
    unittest.main()
