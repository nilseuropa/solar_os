from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayLayerPresentTest(unittest.TestCase):
    def test_layer_reaches_driver_framebuffer(self):
        source = (ROOT / "src/services/solar_os_display.c").read_text()
        names = ["display_prepare_layer_canvas", "display_draw_layer_locked",
                 "solar_os_display_present_regions"]
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "display_layer_present.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "layer_present")
            sources = [ROOT / "tests/host/display_layer_present_test.c",
                       ROOT / "src/services/solar_os_display_layer.c",
                       ROOT / "src/services/solar_os_display_scale.c"]
            sources += [ROOT / "components/u8g2/src/clib" / (name + ".c") for name in (
                "u8g2_setup", "u8g2_buffer", "u8g2_font", "u8g2_ll_hvline", "u8g2_hvline",
                "u8g2_intersection", "u8x8_setup", "u8x8_display")]
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-sign-compare",
                "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
                "-idirafter", str(ROOT / "include"),
                "-I", str(ROOT / "components/u8g2/src/clib"), *map(str, sources),
                "-Wl,--gc-sections", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
