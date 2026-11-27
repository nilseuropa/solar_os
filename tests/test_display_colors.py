from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayColorsTest(unittest.TestCase):
    def test_theme_updates_wait_for_presentations_and_reach_expansion_targets(self):
        source = Path(os.environ.get("SOLAR_OS_TEST_DISPLAY_SOURCE",
            ROOT / "src/services/solar_os_display.c")).read_text()
        names = ("display_release_slot_ref", "display_theme_snapshot", "display_colors_begin",
                 "solar_os_display_get_theme", "display_apply_colors", "display_change_colors",
                 "solar_os_display_set_theme_color", "solar_os_display_set_foreground_color",
                 "solar_os_display_set_background_color")
        # Also exercise the previous implementation when checking a regression.
        functions = production_functions(source, tuple(name for name in names
            if re.search(r"\b" + name + r"\s*\(", source)))
        fixture = (ROOT / "tests/host/display_colors_test.inc").read_text()
        with tempfile.TemporaryDirectory() as temp:
            for board in (0, 1):
                binary = Path(temp) / f"colors-{board}"
                subprocess.run(["cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-pthread", "-fsanitize=address,undefined", "-no-pie",
                    f"-DSOLAR_OS_BOARD_HAS_DISPLAY={board}",
                    "-I", str(ROOT / "include"), "-o", str(binary), "-"],
                    input=fixture.replace("/* FUNCTIONS */", functions), text=True, check=True)
                subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
