import re
import subprocess
import tempfile
import unittest
from pathlib import Path
from test_foreground_presenter import production_functions


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

    def test_python_commits_a_pending_frame_before_blocking_for_tui_input(self):
        fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t mp_obj_t;
typedef enum { PYTHON_EVENT_TUI_WRITE, PYTHON_EVENT_TUI_REFRESH } python_event_type_t;
typedef struct { python_event_type_t type; } python_event_t;
static struct {
    void *key_input;
    bool tui_frame_pending;
    bool stop_requested;
} python_app;
static unsigned queued_frames;
static unsigned frames_at_read;

static void python_ui_send_event(const python_event_t *event)
{
    queued_frames += event->type == PYTHON_EVENT_TUI_REFRESH;
}

static uint32_t python_optional_u32(size_t n, const mp_obj_t *args, size_t index, uint32_t fallback)
{
    return index < n ? args[index] : fallback;
}

static mp_obj_t python_read_key(uint32_t timeout)
{
    frames_at_read = queued_frames;
    return timeout;
}
"""
        fixture += production_functions(PYTHON, ("python_tui_send_event",
            "python_tui_send_simple", "solaros_tui_getch", "solaros_gfx_getch"))
        fixture += r"""
int main(void)
{
    python_app.key_input = &python_app;
    const mp_obj_t timeout = 100;
    python_tui_send_simple(PYTHON_EVENT_TUI_WRITE);
    assert(solaros_tui_getch(0, NULL) == 0 && frames_at_read == 0);
    assert(solaros_gfx_getch(1, &timeout) == timeout && frames_at_read == 0);
    assert(solaros_tui_getch(1, &timeout) == timeout && frames_at_read == 1);
    assert(solaros_tui_getch(1, &timeout) == timeout && frames_at_read == 1);

    python_tui_send_simple(PYTHON_EVENT_TUI_WRITE);
    python_tui_send_simple(PYTHON_EVENT_TUI_REFRESH);
    assert(solaros_tui_getch(1, &timeout) == timeout && frames_at_read == 2);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory, "getch.c"), Path(directory, "getch")
            source.write_text(fixture)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_lua_retains_one_tui_until_refresh_or_shutdown(self):
        self.assertIn("solar_os_tui_t tui;", LUA)
        self.assertIn("solar_os_tui_screen_begin(&solua.tui, ctx)", LUA)
        self.assertIn("solar_os_tui_refresh(tui);", LUA)
        self.assertGreaterEqual(LUA.count("solar_os_tui_end(&solua.tui)"), 2)
        self.assertIn("SOLUA_DRAIN_TUI_EVENTS_PER_TICK 128U", LUA)


if __name__ == "__main__":
    unittest.main()
