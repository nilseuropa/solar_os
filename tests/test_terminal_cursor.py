from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class TerminalCursorTest(unittest.TestCase):
    def test_cursor_pixels_and_blink_lifecycle(self):
        component = ROOT / "components/u8g2"
        cmake = (component / "CMakeLists.txt").read_text().split("\n)", 1)[0]
        sources = [str(component / name) for name in re.findall(r'"([^"]+\.c)"', cmake)]
        with tempfile.TemporaryDirectory() as tmp:
            terminal = (ROOT / "src/services/solar_os_terminal.c").read_text()
            Path(tmp, "terminal_cursor.inc").write_text(production_functions(terminal, (
                "solar_os_terminal_mark_dirty", "terminal_wake_cursor", "terminal_cursor_is_drawn",
                "solar_os_terminal_set_cursor", "solar_os_terminal_set_cursor_visible",
                "solar_os_terminal_cursor_visible", "solar_os_terminal_needs_draw",
                "terminal_apply_polarity", "terminal_set_draw_color", "terminal_draw_cell",
                "terminal_draw_line", "terminal_cell_top_y", "terminal_draw_vrules",
                "cursor_x_position", "terminal_render_hash_bytes", "terminal_render_hash_value",
                "terminal_render_profile_hash", "terminal_render_row_hash",
                "terminal_render_status_hash", "terminal_render_footer_hash",
                "terminal_clear_text_row", "solar_os_terminal_draw",
            )))
            preferences = (ROOT / "src/services/solar_os_terminal_preferences.c").read_text()
            commands = (ROOT / "src/shell/solar_os_shell_commands.c").read_text()
            Path(tmp, "setterm_cursor.inc").write_text(production_functions(preferences, (
                "solar_os_terminal_cursor_shape_name", "solar_os_terminal_parse_cursor_shape",
            )) + production_functions(commands, ("setterm_cursor",)))
            binary = str(Path(tmp, "cursor-test"))
            subprocess.run([
                "cc", "-std=c11", "-O2", "-ffunction-sections", "-fdata-sections",
                "-I" + str(component / "src/clib"), "-I" + tmp,
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/terminal_cursor_test.c"),
                *sources, "-Wl,--gc-sections", "-o", binary,
            ], check=True)
            subprocess.run([binary], check=True)

    def test_saved_preferences_and_invalid_values(self):
        subprocess.run(["make", "-C", str(ROOT / "tests/host"),
                        "terminal_preferences_test", "terminal_preferences_sticky_test"], check=True)
        for name in ("terminal_preferences_test", "terminal_preferences_sticky_test"):
            binary = str(ROOT / "tests/host" / name)
            subprocess.run([binary], check=True)
            for value in (0, 1, 2, 3, 99):
                subprocess.run([binary, str(value)], check=True)


if __name__ == "__main__":
    unittest.main()
