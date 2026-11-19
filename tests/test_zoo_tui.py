from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ZooTuiTest(unittest.TestCase):
    def test_shared_tree_dialogs_geometry_and_worker_lifetime(self):
        source = (ROOT / "src/apps/solar_os_zoo_app.c").read_text()
        source = source[source.index("typedef enum {"):source.index("static void title(")]
        widgets = production_functions((ROOT / "src/services/solar_os_tui_widgets.c").read_text(), (
            "solar_os_tui_screen_fullscreen", "solar_os_tui_screen_bottom_rows",
            "solar_os_tui_screen_content_rows", "solar_os_tui_screen_content_end",
            "solar_os_tui_screen_key", "solar_os_tui_viewport_reconcile", "solar_os_tui_viewport_key"))
        fixture = (ROOT / "tests/host/zoo_tui_test.inc").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "test.c")
            path.write_text(fixture.replace("/* WIDGETS */", widgets).replace("/* APP */", source))
            binary = Path(tmp, "zoo-tui")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                "-include", str(ROOT / "tests/host/compat.h"), "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"),
                str(path), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
