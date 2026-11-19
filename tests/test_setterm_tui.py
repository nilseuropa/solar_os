from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class SettermTuiTest(unittest.TestCase):
    def test_page_navigation_uses_visible_rows(self):
        source = (ROOT / "src/shell/solar_os_shell_setterm_tui.c").read_text()
        enum_start = source.index("typedef enum {")
        enum_end = source.index("} setterm_tui_item_t;", enum_start) + len("} setterm_tui_item_t;")
        harness = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_keys.h"
#include "solar_os_tui_widgets.h"
''' + source[enum_start:enum_end] + r'''
static struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected, first_visible;
    bool editing;
} setterm_tui;
static size_t rows;
static unsigned renders, edit_keys, finishes;
size_t solar_os_tui_rows(const solar_os_tui_t *tui) { (void)tui; return rows; }
static void setterm_tui_set_status(const char *text) { assert(!*text); }
static void setterm_tui_render(void) {
    renders++;
    size_t visible = solar_os_tui_screen_content_rows(&setterm_tui.tui, 1, 1);
    if (visible) {
        assert(setterm_tui.selected >= setterm_tui.first_visible);
        assert(setterm_tui.selected < setterm_tui.first_visible + visible);
        assert(setterm_tui.selected < SETTERM_TUI_ITEM_COUNT);
    }
}
static void setterm_tui_handle_edit_key(char ch) { (void)ch; edit_keys++; }
static void setterm_tui_begin_edit(void) { setterm_tui.editing = true; }
void solar_os_context_finish(solar_os_context_t *ctx, int status, const char *result)
{ (void)ctx; (void)status; (void)result; finishes++; }
''' + production_functions(source, ("setterm_tui_event",)) + r'''
static void key(uint8_t ch) {
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = (char)ch};
    assert(setterm_tui_event(NULL, &event));
}
int main(void) {
    for (unsigned fullscreen = 0; fullscreen < 2; fullscreen++) {
        memset(&setterm_tui, 0, sizeof(setterm_tui));
        setterm_tui.tui.screen_active = true;
        setterm_tui.tui.fullscreen = fullscreen;
        rows = 8;
        size_t page = fullscreen ? 7 : 6;
        key(SOLAR_OS_KEY_PAGE_DOWN);
        assert(setterm_tui.selected == page);
        key(SOLAR_OS_KEY_PAGE_DOWN);
        assert(setterm_tui.selected == page * 2);
        key(SOLAR_OS_KEY_PAGE_UP);
        assert(setterm_tui.selected == page);
        key(SOLAR_OS_KEY_DOWN);
        assert(setterm_tui.selected == page + 1);
        key(SOLAR_OS_KEY_UP);
        assert(setterm_tui.selected == page);
        key(SOLAR_OS_KEY_PAGE_UP);
        assert(setterm_tui.selected == 0);
        const unsigned before = renders;
        key(SOLAR_OS_KEY_PAGE_UP);
        assert(setterm_tui.selected == 0 && renders == before);
        for (unsigned i = 0; i < SETTERM_TUI_ITEM_COUNT; i++) key(SOLAR_OS_KEY_PAGE_DOWN);
        assert(setterm_tui.selected == SETTERM_TUI_ITEM_COUNT - 1);
        /* A resize changes the page size, including a page larger than the form. */
        rows = 64;
        key(SOLAR_OS_KEY_PAGE_UP);
        assert(setterm_tui.selected == 0 && setterm_tui.first_visible == 0);
        key(SOLAR_OS_KEY_PAGE_DOWN);
        assert(setterm_tui.selected == SETTERM_TUI_ITEM_COUNT - 1);
        key('\n');
        const size_t selected = setterm_tui.selected;
        const unsigned edits_before = edit_keys;
        key(SOLAR_OS_KEY_PAGE_UP);
        key(SOLAR_OS_KEY_PAGE_DOWN);
        assert(setterm_tui.editing && setterm_tui.selected == selected && edit_keys == edits_before + 2);
        setterm_tui.editing = false;
        rows = 1;
        const unsigned renders_before = renders;
        key(SOLAR_OS_KEY_PAGE_DOWN);
        key(SOLAR_OS_KEY_PAGE_UP);
        assert(setterm_tui.selected == selected && renders == renders_before);
        key(SOLAR_OS_KEY_ESCAPE);
    }
    assert(finishes == 2);
    puts("setterm page navigation, boundaries, resize, fullscreen and edit isolation passed");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory, "setterm.c")
            path.write_text(harness)
            for packages in (0, 1):
                binary = Path(directory, f"setterm-{packages}")
                subprocess.run([
                    "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-ffunction-sections", "-fdata-sections",
                    "-D_GNU_SOURCE", "-include", str(ROOT / "tests/host/compat.h"),
                    f"-DSOLAR_OS_PACKAGE_SERVICE_BLE={packages}",
                    f"-DSOLAR_OS_PACKAGE_SERVICE_OTA={packages}",
                    "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                    "-I" + str(ROOT / "src/services"), "-idirafter", str(ROOT / "include"),
                    "-I" + str(ROOT / "src/shell"),
                    str(path), str(ROOT / "src/services/solar_os_tui_widgets.c"),
                    "-Wl,--gc-sections", "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
