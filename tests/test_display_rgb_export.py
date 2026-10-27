from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayRgbExportTest(unittest.TestCase):
    def test_wire_order_rotation_and_reader_ownership(self):
        source = (ROOT / "src/services/solar_os_display.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "export.inc").write_text(production_functions(source, (
                "display_export_mono_rgb565", "display_surface_logical_to_native",
                "display_publish_surface")))
            binary = Path(tmp, "export")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/display_rgb_export_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
