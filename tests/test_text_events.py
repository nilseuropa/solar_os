from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


def run_c(source, name):
    with tempfile.TemporaryDirectory() as tmp:
        Path(tmp, name + ".c").write_text(source)
        binary = Path(tmp, name)
        subprocess.run([
            "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-include", str(ROOT / "tests/host/compat.h"),
            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src"),
            "-I", str(ROOT / "src/services"), "-idirafter", str(ROOT / "include"),
            str(Path(tmp, name + ".c")),
            "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


class TextEventsTest(unittest.TestCase):
    def test_local_text_reaches_text_event_apps_as_utf8(self):
        source = r'''
#include <assert.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_key_codes.h"

static bool alt_prefix_pending;
static solar_os_app_t input_app;
static solar_os_event_t dispatched;
static int dispatch_count;
static int request_count;

static const solar_os_app_t *solar_os_sessions_input_app(void)
{
    return &input_app;
}

static bool solar_os_sessions_dispatch_input_event(const solar_os_event_t *event)
{
    dispatched = *event;
    dispatch_count++;
    return true;
}

static void process_app_requests(void)
{
    request_count++;
}
'''
        source += production_functions((ROOT / "src/main.c").read_text(),
                                        ("input_focus_accepts_text_events",
                                         "dispatch_text_to_input_focus"))
        source += r'''
static void expect_text(uint8_t key, const char *text)
{
    dispatch_count = 0;
    request_count = 0;
    assert(dispatch_text_to_input_focus(key));
    assert(dispatch_count == 1 && request_count == 1);
    assert(dispatched.type == SOLAR_OS_EVENT_TEXT);
    assert(strcmp(dispatched.data.text, text) == 0);
}

static void expect_legacy(uint8_t key)
{
    dispatch_count = 0;
    assert(!dispatch_text_to_input_focus(key));
    assert(dispatch_count == 0);
}

int main(void)
{
    input_app.flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_TEXT_EVENTS;
    expect_text(' ', " ");
    expect_text('a', "a");
    expect_text('~', "~");
    expect_text(0xc4, "\xc3\x84");
    expect_text(0xdf, "\xc3\x9f");
    expect_text(0xe4, "\xc3\xa4");
    expect_text(0xff, "\xc3\xbf");
    expect_legacy('\n');
    expect_legacy('\b');
    expect_legacy(0x13);
    expect_legacy(SOLAR_OS_KEY_ESCAPE);
    expect_legacy(0x7f);
    expect_legacy(SOLAR_OS_KEY_UP);
    expect_legacy(SOLAR_OS_KEY_APP_EXIT);
    expect_legacy(SOLAR_OS_KEY_ALT_PREFIX);
    expect_legacy(SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE);
    expect_legacy(0xbf);

    alt_prefix_pending = true;
    expect_legacy('a');
    alt_prefix_pending = false;

    input_app.flags = SOLAR_OS_APP_FLAG_RESUMABLE;
    expect_legacy('a');
    expect_legacy(0xe4);
    return 0;
}
'''
        run_c(source, "text_dispatch_test")

    def test_writer_inserts_text_events_and_routes_dialog_ascii(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "solar_os.h"

typedef enum {
    WRITER_DIALOG_NONE,
    WRITER_DIALOG_FIND,
} writer_dialog_t;

static struct {
    writer_dialog_t dialog;
} writer;

static char inserted[16];
static size_t inserted_len;
static int insert_count;
static uint8_t dialog_keys[4];
static size_t dialog_key_count;

static void writer_wake_cursor(void)
{
}

static esp_err_t writer_insert(const char *text, size_t text_len)
{
    assert(inserted_len + text_len <= sizeof(inserted));
    memcpy(&inserted[inserted_len], text, text_len);
    inserted_len += text_len;
    insert_count++;
    return ESP_OK;
}

static bool writer_handle_dialog(solar_os_context_t *ctx, uint8_t ch)
{
    (void)ctx;
    assert(dialog_key_count < sizeof(dialog_keys));
    dialog_keys[dialog_key_count++] = ch;
    return true;
}
'''
        source += production_functions((ROOT / "src/apps/solar_os_writer.c").read_text(),
                                        ("writer_handle_text",))
        source += r'''
int main(void)
{
    solar_os_context_t ctx = {0};
    assert(writer_handle_text(&ctx, "a"));
    assert(writer_handle_text(&ctx, "\xc3\xa4"));
    assert(writer_handle_text(&ctx, ""));
    assert(insert_count == 2);
    assert(inserted_len == 3 && memcmp(inserted, "a\xc3\xa4", 3) == 0);

    writer.dialog = WRITER_DIALOG_FIND;
    assert(writer_handle_text(&ctx, "q"));
    assert(writer_handle_text(&ctx, "\xc3\xa4"));
    assert(dialog_key_count == 1 && dialog_keys[0] == 'q');
    assert(insert_count == 2);
    return 0;
}
'''
        run_c(source, "writer_text_test")


if __name__ == "__main__":
    unittest.main()
