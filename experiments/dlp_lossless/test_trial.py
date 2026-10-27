"""Reference-LZ4 differential checks and production receiver interoperability."""
import ctypes
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from benchmark import Lz4, plan, scenes

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tests"))
sys.path.insert(0, str(ROOT / "scripts"))
import dlp_image as dlp
from test_dlp_image import Clock
from test_dlp_window import Port


class TrialTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.directory.cleanup)
        directory = Path(cls.directory.name)
        wrapper = directory / "wrapper.c"
        wrapper.write_text('#include "decoder.h"\n'
            'bool decode(uint8_t *out, size_t n, const uint8_t *in, size_t size) {\n'
            'return solar_os_dlp_trial_lz4_decode(out, n, in, size); }\n')
        library = directory / "decoder.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-I" + str(HERE), str(wrapper), "-o", str(library)], check=True)
        cls.decoder = ctypes.CDLL(str(library)).decode
        cls.decoder.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        cls.decoder.restype = ctypes.c_bool
        cls.lz4 = Lz4()
        library = directory / "receiver.so"
        subprocess.run(["cc", "-shared", "-fPIC", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-DSOLAR_OS_DLP_LOSSLESS_TRIAL=1", "-fsanitize=undefined",
                        "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "protocol/display_link/include"),
                        str(ROOT / "tests/host/dlp_window_bridge.c"),
                        str(ROOT / "src/services/solar_os_dlp_receiver.c"),
                        str(ROOT / "protocol/display_link/src/sdlp.c"), "-o", str(library)], check=True)
        cls.bridge = ctypes.CDLL(str(library))
        cls.bridge.bridge_feed.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        cls.bridge.bridge_tick.argtypes = [ctypes.c_uint64]
        cls.bridge.bridge_read.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.bridge.bridge_read.restype = ctypes.c_size_t
        cls.bridge.sdlp_rle_decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                              ctypes.c_void_p, ctypes.c_size_t]
        cls.bridge.sdlp_rle_decode.restype = ctypes.c_bool

    def decode(self, data, expected):
        output = ctypes.create_string_buffer(expected + 32)
        ctypes.memset(output, 0xa5, len(output))
        ok = self.decoder(ctypes.byref(output, 16), expected, data, len(data))
        self.assertEqual(output.raw[:16], b"\xa5" * 16)
        self.assertEqual(output.raw[-16:], b"\xa5" * 16)
        return ok, output.raw[16:16 + expected]

    def test_reference_blocks_and_corrupt_inputs(self):
        rng = random.Random(72613)
        count = 0
        for size in (*range(1, 70), 127, 128, 255, 256, 511, 512, 1000, 1024):
            for source in (bytes(size), b"\xf8\0" * (size // 2) + b"\xff" * (size % 2),
                           bytes(k % 256 for k in range(size)), rng.randbytes(size)):
                encoded = self.lz4.encode(source)
                ok, decoded = self.decode(encoded, size)
                self.assertTrue(ok)
                self.assertEqual(decoded, source)
                count += 1
                for _ in range(8):
                    corrupt = bytearray(encoded)
                    at = rng.randrange(len(corrupt))
                    corrupt[at] ^= 1 << rng.randrange(8)
                    ok, decoded = self.decode(bytes(corrupt), size)
                    if ok:
                        reference = ctypes.create_string_buffer(size)
                        n = self.lz4.lib.LZ4_decompress_safe(bytes(corrupt), reference, len(corrupt), size)
                        if n == size:
                            self.assertEqual(decoded, reference.raw)
                for end in range(min(len(encoded), 32)):
                    self.decode(encoded[:end], size)
        for _ in range(20000):
            data = rng.randbytes(rng.randrange(1, 207))
            self.decode(data, rng.randrange(1, 1025))
        print(f"Checked {count} valid reference blocks, mutations, truncations, and 20000 random packets")

    def test_production_receiver_lz4_window(self):
        clock = Clock()
        with patch.object(dlp.time, "monotonic", clock.monotonic), patch.object(dlp.time, "sleep", clock.sleep):
            for compact in (False, True):
                for fault in (None, "loss", "duplicate", "corrupt", "reorder", "ack", "completion"):
                    with self.subTest(compact=compact, fault=fault):
                        self.bridge.bridge_init(16)
                        port = Port(self.bridge, clock, fault)
                        link = dlp.DisplayLink(port, timeout=.5, compact=compact)
                        self.assertEqual(link.connect(), (64, 32))
                        image = b"\xf8\0\xaa\x55" * 64
                        link._upload_command(dlp.BEGIN_FRAME, struct.pack("<II", 1, 1))
                        link._upload_command(dlp.BEGIN_RECT, struct.pack("<IIHHHH", 1, 1, 0, 0, 64, 32))
                        encoded = self.lz4.encode(image)
                        link._upload_command(0x16, struct.pack("<IIIH", 1, 1, 0, len(image)) + encoded)
                        link._upload_command(dlp.PRESENT, struct.pack("<I", 1))
                        self.assertEqual(self.bridge.bridge_presentations(), 1)
                        shown = ctypes.create_string_buffer(256)
                        self.bridge.bridge_image(shown)
                        self.assertEqual(shown.raw, image)
                        version, limit, checksum, size = struct.unpack("<HHII", link.command(8).payload)
                        expected = 2166136261
                        for value in image:
                            expected = ((expected ^ value) * 16777619) & 0xffffffff
                        self.assertEqual((version, limit, checksum, size), (1, 1024, expected, 256))

    def test_plans_reconstruct_exact_rgb565(self):
        width, height = 73, 35
        for name, frames in scenes(width, height).items():
            for mode in ("raw-full", "rle-full", "rle-bands", "rle-tiles", "lz4-full", "lz4-tiles"):
                with self.subTest(scene=name, mode=mode):
                    p = plan(frames[0], frames[1], width, height, mode, 1024, self.lz4)
                    output = bytearray(frames[0])
                    for op, rect_id, data in p[1]:
                        if op == dlp.BEGIN_RECT:
                            x, y, w, h = struct.unpack("<HHHH", data)
                            self.assertLessEqual(x + w, width)
                            self.assertLessEqual(y + h, height)
                            continue
                        offset = struct.unpack_from("<I", data)[0]
                        block = data[4:]
                        if op != dlp.RECT_DATA:
                            expected = struct.unpack_from("<H", block)[0]
                            compressed = block[2:]
                            decoded = ctypes.create_string_buffer(expected)
                            if op == dlp.RECT_RLE:
                                self.assertTrue(self.bridge.sdlp_rle_decode(
                                    decoded, expected, compressed, len(compressed)))
                            else:
                                self.assertEqual(op, 0x16)
                                self.assertEqual(self.lz4.lib.LZ4_decompress_safe(
                                    compressed, decoded, len(compressed), expected), expected)
                            block = decoded.raw
                        self.assertLessEqual(offset + len(block), w * h * 2)
                        for k, value in enumerate(block):
                            row, col = divmod(offset + k, w * 2)
                            output[(y + row) * width * 2 + x * 2 + col] = value
                    self.assertEqual(bytes(output), frames[1])


if __name__ == "__main__":
    unittest.main()
