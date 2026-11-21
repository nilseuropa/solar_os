from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class TelnetClientTest(unittest.TestCase):
    def test_keys_are_sent_whole_and_telnet_escaping_is_preserved(self):
        functions = production_functions(
            (ROOT / "src/apps/solar_os_telnet.c").read_text(),
            ("telnet_send_bytes_escaped", "telnet_send_key"),
        )
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "solar_os_key_codes.h"

#define TELNET_IAC 255U
static struct { bool raw_mode, alt_prefix_pending; } telnet_app;
static uint8_t output[1024];
static size_t output_len, writes;
static int telnet_send_raw(const uint8_t *data, size_t len)
{
    assert(output_len + len <= sizeof(output));
    memcpy(output + output_len, data, len);
    output_len += len;
    writes++;
    return 0;
}
''' + functions + r'''
static void reset(void)
{
    output_len = writes = 0;
    telnet_app.alt_prefix_pending = false;
}

static void expect(const uint8_t *bytes, size_t len)
{
    /* A separate write for Escape can exceed the peer's key timeout when
     * TCP delays the remaining bytes until the first write is acknowledged. */
    assert(writes == 1);
    assert(output_len == len && memcmp(output, bytes, len) == 0);
    assert(!telnet_app.alt_prefix_pending);
}

int main(void)
{
    static const struct { uint8_t key; const char *bytes; } keys[] = {
        {SOLAR_OS_KEY_UP, "\x1b[A"},
        {SOLAR_OS_KEY_DOWN, "\x1b[B"},
        {SOLAR_OS_KEY_LEFT, "\x1b[D"},
        {SOLAR_OS_KEY_RIGHT, "\x1b[C"},
        {SOLAR_OS_KEY_PAGE_UP, "\x1b[5~"},
        {SOLAR_OS_KEY_PAGE_DOWN, "\x1b[6~"},
        {SOLAR_OS_KEY_HOME, "\x1b[H"},
        {SOLAR_OS_KEY_END, "\x1b[F"},
        {SOLAR_OS_KEY_DELETE, "\x1b[3~"},
        {SOLAR_OS_KEY_F1, "\x1b[11~"},
        {SOLAR_OS_KEY_F2, "\x1b[12~"},
        {SOLAR_OS_KEY_F3, "\x1b[13~"},
        {SOLAR_OS_KEY_F4, "\x1b[14~"},
        {SOLAR_OS_KEY_F5, "\x1b[15~"},
        {SOLAR_OS_KEY_F6, "\x1b[17~"},
        {SOLAR_OS_KEY_F7, "\x1b[18~"},
        {SOLAR_OS_KEY_F8, "\x1b[19~"},
        {SOLAR_OS_KEY_F9, "\x1b[20~"},
        {SOLAR_OS_KEY_F10, "\x1b[21~"},
        {SOLAR_OS_KEY_F11, "\x1b[23~"},
        {SOLAR_OS_KEY_F12, "\x1b[24~"},
        {'a', "a"}, {'\b', "\x7f"},
    };
    for (unsigned raw = 0; raw < 2; raw++) {
        telnet_app.raw_mode = raw;
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            for (unsigned alt = 0; alt < 2; alt++) {
                reset();
                if (alt) {
                    telnet_send_key((char)SOLAR_OS_KEY_ALT_PREFIX);
                    assert(writes == 0);
                }
                telnet_send_key((char)keys[i].key);
                uint8_t expected[16];
                const size_t len = strlen(keys[i].bytes);
                expected[0] = 0x1b;
                memcpy(expected + alt, keys[i].bytes, len);
                expect(expected, len + alt);
            }
        }
        for (unsigned newline = 0; newline < 2; newline++) {
            reset();
            telnet_send_key(newline ? '\n' : '\r');
            expect((const uint8_t *)"\r\n", raw ? 1 : 2);
        }
        reset();
        const uint8_t data[] = {'a', 0xff, 0xff, 'z'};
        const uint8_t escaped[] = {'a', 0xff, 0xff, 0xff, 0xff, 'z'};
        telnet_send_bytes_escaped(data, sizeof(data));
        expect(raw ? data : escaped, raw ? sizeof(data) : sizeof(escaped));

        /* Exercise bounded buffering with repeated IACs across its edges. */
        reset();
        uint8_t large[257];
        memset(large, 0xff, sizeof(large));
        telnet_send_bytes_escaped(large, sizeof(large));
        assert(output_len == sizeof(large) * (raw ? 1 : 2));
        for (size_t i = 0; i < output_len; i++) assert(output[i] == 0xff);

        reset();
        memset(large, 'a', sizeof(large));
        large[31] = large[63] = large[255] = 0xff;
        telnet_send_bytes_escaped(large, sizeof(large));
        size_t position = 0;
        for (size_t i = 0; i < sizeof(large); i++) {
            assert(output[position++] == large[i]);
            if (!raw && large[i] == 0xff) assert(output[position++] == 0xff);
        }
        assert(output_len == position);
    }
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="solaros-telnet-") as directory:
            binary = Path(directory, "telnet-client")
            subprocess.run(
                ["cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                 "-I", str(ROOT / "include"), "-o", str(binary), "-"],
                input=code, text=True, check=True,
            )
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
