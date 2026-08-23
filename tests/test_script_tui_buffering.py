import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
TUI = (REPOSITORY / "src/services/solar_os_tui.c").read_text(encoding="utf-8")
PYTHON = (REPOSITORY / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")


class ScriptTuiBufferingTest(unittest.TestCase):
    def test_diff_refresh_is_available_to_display_and_port_terminals(self):
        refresh = re.search(
            r"void solar_os_tui_refresh\(solar_os_tui_t \*tui\)\s*\{(.*?)\n\}",
            TUI,
            re.DOTALL,
        )
        self.assertIsNotNone(refresh)
        self.assertIn("tui->diff_enabled", refresh.group(1))
        self.assertIn("solar_os_shell_io_is_cursor_addressable", refresh.group(1))
        self.assertNotIn("SOLAR_OS_SHELL_IO_KIND_PORT", refresh.group(1))

    def test_python_retains_one_tui_until_refresh_or_shutdown(self):
        self.assertIn("solar_os_tui_t tui;", PYTHON)
        self.assertIn("solar_os_tui_screen_begin(&python_app.tui, ctx)", PYTHON)
        self.assertIn("solar_os_tui_refresh(tui);", PYTHON)
        self.assertGreaterEqual(PYTHON.count("solar_os_tui_end(&python_app.tui)"), 2)
        self.assertIn("PYTHON_DRAIN_TUI_EVENTS_PER_TICK 128U", PYTHON)

    def test_lua_retains_one_tui_until_refresh_or_shutdown(self):
        self.assertIn("solar_os_tui_t tui;", LUA)
        self.assertIn("solar_os_tui_screen_begin(&solua.tui, ctx)", LUA)
        self.assertIn("solar_os_tui_refresh(tui);", LUA)
        self.assertGreaterEqual(LUA.count("solar_os_tui_end(&solua.tui)"), 2)
        self.assertIn("SOLUA_DRAIN_TUI_EVENTS_PER_TICK 128U", LUA)


if __name__ == "__main__":
    unittest.main()
