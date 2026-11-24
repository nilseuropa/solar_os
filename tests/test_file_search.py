"""Exercise the production quick-search routing for local and network panes."""
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def block(source, start):
    begin = source.index(start)
    brace = source.index("{", begin)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_file_search.h"
#include "solar_os_file_shortcuts.h"
typedef int solar_os_context_t;
enum {SOLAR_OS_EVENT_CHAR, SOLAR_OS_EVENT_KEY, SOLAR_OS_EVENT_TICK};
typedef struct {
    int type;
    union {int ch; solar_os_input_key_event_t key;} data;
} solar_os_event_t;
enum {FILES_INPUT_NONE, FILES_INPUT_SEARCH, FILES_INPUT_MKDIR};
typedef struct {char name[32];} files_entry_t;
typedef struct {files_entry_t *entries; size_t count, cursor;} files_pane_t;
static struct {
    int tui, input_mode;
    size_t active;
    bool alt_prefix_pending, busy;
    files_pane_t panes[2];
    solar_os_text_search_state_t search[2];
    struct {bool running, cancel_requested;} worker;
    struct {bool cancelling;} transaction;
} files;
static int dispatched, finished;
#define SOLAR_OS_TUI_SCREEN_KEY_TOGGLED 1
static int solar_os_tui_screen_key(int *tui, int ch) {(void)tui; (void)ch; return 0;}
static void files_render() {}
static void files_set_message(const char *text) {(void)text;}
static void solar_os_context_finish(solar_os_context_t *ctx, int result, void *data)
{(void)ctx; (void)result; (void)data; finished++;}
/* STUBS */
/* SEARCH */
/* INPUT */
/* EVENT */
static void character(int ch)
{
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_CHAR, .data.ch = ch};
    dispatched = -1;
    files_event(NULL, &event);
}
static void key(int ch, int modifiers, int action)
{
    solar_os_event_t event = {.type = SOLAR_OS_EVENT_KEY, .data.key = {
        .key = ch, .usage = SOLAR_OS_FILE_SHORTCUT_SEARCH_USAGE,
        .modifiers = modifiers, .action = action}};
    dispatched = -1;
    files_event(NULL, &event);
}
int main(void)
{
    files_entry_t entries[] = {{".."}, {"alpha"}, {"alpine"}, {"myalpha"}, {"beta"}};
    files.panes[0] = (files_pane_t){.entries = entries, .count = 5};
    files.panes[1] = files.panes[0];
    character(0x13); /* Port shell Ctrl+S. */
    assert(files.input_mode == FILES_INPUT_SEARCH);
    character('a'); character('l');
    assert(files.panes[0].cursor == 1);
    key('s', SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_PRESS);
    assert(files.panes[0].cursor == 2);
    assert(!strcmp(files.search[0].input, "al")); /* Alt+S is not typed as 's'. */
    key('s', SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_RELEASE);
    assert(files.panes[0].cursor == 2);
    key(SOLAR_OS_KEY_CTRL, 0, SOLAR_OS_INPUT_KEY_PRESS);
    key(0x13, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_PRESS);
    assert(files.panes[0].cursor == 1);
    key(0x13, SOLAR_OS_INPUT_MOD_LEFT_CTRL, SOLAR_OS_INPUT_KEY_REPEAT);
    assert(files.panes[0].cursor == 2);
    character(SOLAR_OS_KEY_ALT_PREFIX); character('s');
    assert(files.panes[0].cursor == 1 && !strcmp(files.search[0].input, "al"));
    character(SOLAR_OS_KEY_ENTER);
    assert(files.input_mode == FILES_INPUT_NONE && dispatched == SOLAR_OS_KEY_ENTER);
    assert(files.panes[0].cursor == 1 && !finished);
    character(0x13); character(0x13); /* Recall and advance. */
    assert(files.panes[0].cursor == 2 && !strcmp(files.search[0].input, "al"));
    character('\t');
    assert(dispatched == '\t' && files.input_mode == FILES_INPUT_NONE);
    files.active = 1; /* The app's normal Tab command switches panes. */
    character(0x13); character('b');
    assert(files.panes[1].cursor == 4 && !strcmp(files.search[1].input, "b"));
    character(SOLAR_OS_KEY_ESCAPE);
    assert(files.input_mode == FILES_INPUT_NONE && dispatched == -1 && !finished);
    assert(!strcmp(files.search[0].query, "al") && !strcmp(files.search[1].query, "b"));
    character(0x13); character('a');
    character(SOLAR_OS_KEY_F3);
    assert(dispatched == SOLAR_OS_KEY_F3 && files.input_mode == FILES_INPUT_NONE);
    files.input_mode = FILES_INPUT_MKDIR;
    key('s', SOLAR_OS_INPUT_MOD_LEFT_ALT, SOLAR_OS_INPUT_KEY_PRESS);
    assert(files.input_mode == FILES_INPUT_MKDIR); /* Don't replace another prompt. */
    return 0;
}
'''


class FileSearchTest(unittest.TestCase):
    def test_local_and_network_event_routing(self):
        for app in ("files", "ftp_app"):
            with self.subTest(app=app), tempfile.TemporaryDirectory() as directory:
                source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
                source = source.replace("ftp_app", "files").replace("FTP_APP_INPUT", "FILES_INPUT")
                search = block(source, "static bool files_search_segment")
                search += block(source, "static void files_begin_search")
                input_source = source[source.index("static bool files_input_event"):]
                search_branch = block(input_source, "if (files.input_mode == FILES_INPUT_SEARCH)")
                signature = "static bool files_input_event(" + (
                    "solar_os_context_t *ctx, uint8_t ch" if app == "files" else "uint8_t ch"
                ) + ") {\n"
                input_handler = signature + ("(void)ctx;\n" if app == "files" else "")
                input_handler += search_branch + "\nreturn files.input_mode != FILES_INPUT_NONE;\n}"
                event = block(source, "static bool files_event")
                event = event.split("\n    switch (ch)", 1)[0]
                event += "\n(void)pane; dispatched = ch; return true;\n}"
                stubs = "static files_pane_t *files_active_pane(void) {return &files.panes[files.active];}\n" if app == "files" else ""
                stubs += "static void " + ("files_worker_poll" if app == "files" else "files_poll")
                stubs += "(solar_os_context_t *ctx) {(void)ctx;}\n"
                harness = HARNESS.replace("/* STUBS */", stubs).replace("/* SEARCH */", search)
                harness = harness.replace("/* INPUT */", input_handler).replace("/* EVENT */", event)
                path = Path(directory)
                (path / "test.c").write_text(harness)
                compiled = subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
                    "-I", str(ROOT / "src/apps"), "-I", str(ROOT / "src/services"),
                    str(path / "test.c"), str(ROOT / "src/services/solar_os_text_search.c"),
                    "-o", str(path / "test"),
                ], capture_output=True, text=True)
                self.assertEqual(compiled.returncode, 0, compiled.stderr)
                subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
