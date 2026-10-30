from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class FuncgenLayoutTest(unittest.TestCase):
    def test_graphics_and_short_terminal_controls_fit(self):
        source = (ROOT / "src/apps/solar_os_funcgen.c").read_text()
        control = re.search(r"typedef enum \{[^}]*\} funcgen_control_t;", source).group()
        functions = production_functions(source, (
            "funcgen_control_label", "funcgen_draw_text_fit", "funcgen_draw_knob",
            "funcgen_render_compact", "funcgen_render_graphics", "funcgen_render_tui",
        ))
        viewport = production_functions(
            (ROOT / "src/services/solar_os_tui_widgets.c").read_text(),
            ("solar_os_tui_viewport_reconcile",))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "controls.inc").write_text(control)
            Path(tmp, "funcgen_layout.inc").write_text(viewport + "\n" + functions)
            binary = Path(tmp, "layout")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                str(ROOT / "tests/host/funcgen_layout_test.c"), "-o", str(binary),
            ], check=True)
            preview = os.environ.get("SOLAROS_FUNCGEN_PREVIEW_DIR")
            if preview:
                Path(preview).mkdir(parents=True, exist_ok=True)
            subprocess.run([str(binary), *([preview] if preview else [])], check=True)
