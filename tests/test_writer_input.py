from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class WriterInputTest(unittest.TestCase):
    def test_layout_characters_are_inserted_as_utf8(self):
        source = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "solar_os_key_codes.h"

static char inserted[16];
static size_t inserted_len;

static esp_err_t writer_insert(const char *text, size_t text_len)
{
    assert(inserted_len + text_len <= sizeof(inserted));
    memcpy(&inserted[inserted_len], text, text_len);
    inserted_len += text_len;
    return ESP_OK;
}
'''
        source += production_functions((ROOT / "src/apps/solar_os_writer.c").read_text(),
                                        ("writer_insert_key",))
        source += r'''
static void expect(uint8_t key, const char *bytes, size_t len)
{
    inserted_len = 0;
    writer_insert_key(key);
    assert(inserted_len == len);
    assert(memcmp(inserted, bytes, len) == 0);
}

int main(void)
{
    expect('a', "a", 1);
    expect('~', "~", 1);
    expect(0xe4, "\xc3\xa4", 2);
    expect(0xc4, "\xc3\x84", 2);
    expect(0xdf, "\xc3\x9f", 2);
    expect(0xfc, "\xc3\xbc", 2);
    expect(0x7f, "", 0);
    expect(SOLAR_OS_KEY_F2, "", 0);
    expect(SOLAR_OS_KEY_ALT_PREFIX, "", 0);
    expect(SOLAR_OS_KEY_CTRL_DOWN, "", 0);
    expect(SOLAR_OS_KEY_CTRL_SHIFT_UP, "", 0);
    expect(SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE, "", 0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "writer_input_test.c").write_text(source)
            binary = Path(tmp, "writer-input")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
                str(Path(tmp, "writer_input_test.c")),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
