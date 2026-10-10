import importlib.util
import io
import math
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
SOLAROS = types.SimpleNamespace(time=types.SimpleNamespace(sleep_ms=lambda _: None))
SPEC = importlib.util.spec_from_file_location("classify_example", ROOT / "examples/python/classify.py")
CLASSIFY = importlib.util.module_from_spec(SPEC)
with patch.dict(sys.modules, solaros=SOLAROS):
    SPEC.loader.exec_module(CLASSIFY)


class ClassifyExampleTest(unittest.TestCase):
    def picture(self, data):
        with tempfile.NamedTemporaryFile() as source:
            source.write(data)
            source.flush()
            return CLASSIFY.read_portable_picture(source.name)

    def test_ppm_resize_orientation_and_binary_whitespace_pixels(self):
        pixels = bytes((10, 32, 13, 255, 0, 0, 0, 255, 0, 0, 0, 255))
        data, size = self.picture(b"P6\n# comment\n2 2\n255\n" + pixels)
        self.assertEqual(size, (2, 2))
        self.assertEqual(len(data), 224 * 224 * 3)
        self.assertEqual(data[:3], pixels[:3])
        self.assertEqual(data[112 * 3:112 * 3 + 3], pixels[3:6])
        self.assertEqual(data[112 * 224 * 3:112 * 224 * 3 + 3], pixels[6:9])
        self.assertEqual(data[-3:], pixels[9:12])
        crlf, _ = self.picture(b"P6\r\n2 2\r\n255\r\n" + pixels)
        self.assertEqual(crlf, data)

    def test_bmp_padding_top_down_and_bottom_up_match(self):
        # Two 24-bit pixels per row plus two padding bytes; BMP stores BGR.
        red_green = b"\x00\x00\xff\x00\xff\x00\x00\x00"
        blue_white = b"\xff\x00\x00\xff\xff\xff\x00\x00"
        def bmp(height, rows):
            dib = struct.pack("<IiiHHIIiiII", 40, 2, height, 1, 24, 0, 16, 0, 0, 0, 0)
            return b"BM" + struct.pack("<IHHI", 70, 0, 0, 54) + dib + rows
        bottom, _ = self.picture(bmp(2, blue_white + red_green))
        top, _ = self.picture(bmp(-2, red_green + blue_white))
        self.assertEqual(top, bottom)
        self.assertEqual(top[:3], b"\xff\x00\x00")
        self.assertEqual(top[-3:], b"\xff\xff\xff")
        with self.assertRaisesRegex(ValueError, "truncated"):
            self.picture(bmp(2, b""))

    def test_invalid_image_headers_are_rejected(self):
        for data in (b"P6\n0 1\n255\n", b"P6\n3 3\n65535\n", b"\xff\xd8"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                self.picture(data)

    def test_quantization_saturation_and_channel_order(self):
        tables = CLASSIFY.quantization_tables(-6)
        data = bytearray((0, 0, 0, 255, 255, 255, 124, 116, 104))
        original = data[:]
        CLASSIFY.quantize_in_place(data, tables)
        for index, value in enumerate(original):
            channel = index % 3
            expected = math.floor((value - CLASSIFY.MEAN[channel]) / CLASSIFY.STD[channel] * 64 + .5)
            self.assertEqual(data[index], max(-128, min(127, expected)) & 255)

    def test_softmax_uses_signed_logits_exported_exponent_and_all_classes(self):
        result = CLASSIFY.top_classes(bytes((255, 0, 4)), -2, ["a", "b", "c"], 2)
        total = math.exp(-.25) + 1 + math.exp(1)
        self.assertEqual([r["id"] for r in result], [2, 1])
        self.assertAlmostEqual(result[0]["score"], math.exp(1) / total)
        self.assertAlmostEqual(result[1]["score"], 1 / total)

    def test_native_image_is_closed_on_export_failure(self):
        closed = []
        def fail(*_):
            raise MemoryError()
        image = types.SimpleNamespace(open=lambda _: 7, size=lambda _: (640, 480),
                                      to_rgb=fail, close=closed.append)
        with patch.object(SOLAROS, "image", image, create=True), self.assertRaises(MemoryError):
            CLASSIFY.picture_rgb("photo.jpg")
        self.assertEqual(closed, [7])

    def test_bmp_and_ppm_use_portable_loader_with_native_api_present(self):
        def fail(*_):
            raise AssertionError("native decoder does not support BMP/PPM")
        image = types.SimpleNamespace(open=fail, to_rgb=fail)
        with patch.object(SOLAROS, "image", image, create=True), \
                patch.object(CLASSIFY, "read_portable_picture", return_value=(b"RGB", (1, 1))) as portable:
            self.assertEqual(CLASSIFY.picture_rgb("photo.BMP"), (b"RGB", (1, 1)))
            self.assertEqual(CLASSIFY.picture_rgb("photo.ppm"), (b"RGB", (1, 1)))
            self.assertEqual(portable.call_count, 2)

    def test_model_is_closed_when_picture_processing_fails(self):
        closed = []
        info = {"inputs": {"input.1": {"dtype": "int8", "shape": [1, 224, 224, 3], "exponents": [-6]}},
                "outputs": {"466": {"dtype": "int8", "shape": [1, 1000], "exponents": [-2]}},
                "model_bytes": 3612288, "internal_bytes": 10, "external_bytes": 5000000}
        inference = types.SimpleNamespace(load=lambda *_: 9, info=lambda _: info, close=closed.append)
        with tempfile.NamedTemporaryFile(mode="w") as labels:
            labels.write("\n".join("class%d" % i for i in range(1000)))
            labels.flush()
            with patch.object(SOLAROS, "inference", inference, create=True), \
                    patch.object(SOLAROS.time, "uptime_ms", lambda: 0, create=True), \
                    patch.object(CLASSIFY, "picture_rgb", side_effect=ValueError("invalid picture")), \
                    patch.object(CLASSIFY.gc, "mem_free", lambda: 500000, create=True), \
                    patch("sys.stdout", io.StringIO()), self.assertRaisesRegex(ValueError, "invalid picture"):
                CLASSIFY.main(["/dl/classify.py", "bad.jpg", "--labels", labels.name])
        self.assertEqual(closed, [9])


if __name__ == "__main__":
    unittest.main()
