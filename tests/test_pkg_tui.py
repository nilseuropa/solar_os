from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class PackageTuiTest(unittest.TestCase):
    def compile_run(self, name, code):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp, "test.c")
            source.write_text(code)
            binary = Path(tmp, name)
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "include"), str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, cwd=tmp)

    def test_cached_catalog_and_modal_tree_workflow(self):
        tui = (ROOT / "src/shell/solar_os_shell_pkg_tui.c").read_text()
        tui = tui[tui.index("typedef enum {"):tui.index("static const solar_os_app_t pkg_tui_app")]
        viewport = production_functions((ROOT / "src/services/solar_os_tui_widgets.c").read_text(), (
            "solar_os_tui_viewport_reconcile", "solar_os_tui_viewport_key"))
        fixture = (ROOT / "tests/host/pkg_tui_test.inc").read_text()
        self.compile_run("pkg-tui", fixture.replace("/* VIEWPORT */", viewport).replace("/* TUI */", tui))

    def test_signed_cache_reload_and_failure_preservation(self):
        fixture = (ROOT / "tests/host/module_catalog_cache_test.inc").read_text()
        cache = (ROOT / "src/services/solar_os_module_catalog_cache.inc").read_text()
        decode = production_functions((ROOT / "src/services/solar_os_module_packages.c").read_text(), (
            "module_catalog_decode",))
        self.compile_run("pkg-cache", fixture.replace("/* CACHE */", decode + "\n" + cache))
