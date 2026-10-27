from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayRotationMemoryTest(unittest.TestCase):
    def test_rotation_uses_managed_psram_when_sdk_heap_is_exhausted(self):
        source = (ROOT / "src/drivers/tft_ili9341.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "display_rotation.inc").write_text(production_functions(
                source, ("ili9341_prepare_native_frame",)))
            binary = Path(tmp, "rotation")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"),
                str(ROOT / "tests/host/display_rotation_memory_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
