from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DocUtf8Test(unittest.TestCase):
    def test_clipping_and_render_batches_preserve_utf8_and_positions(self):
        doc = (ROOT / "src/services/solar_os_doc.c").read_text()
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
typedef int solar_os_gfx_t;
static const char *expected;
static size_t expected_len, drawn, calls;
static int expected_x;
'''
        source += production_functions(doc, (
            "doc_utf8_continuation", "doc_utf8_prefix", "doc_utf8_columns",
        ))
        source += r'''
static void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int y, const char *text)
{
    (void)gfx;
    assert(y == 12 && x == expected_x);
    const size_t n = strlen(text);
    assert(n > 0 && drawn + n <= expected_len);
    assert(memcmp(expected + drawn, text, n) == 0);
    assert(!doc_utf8_continuation((unsigned char)text[0]));
    drawn += n;
    assert(drawn == expected_len || !doc_utf8_continuation((unsigned char)expected[drawn]));
    expected_x += (int)doc_utf8_columns(text, n) * 7;
    calls++;
}
'''
        source += production_functions(doc, ("doc_render_text_slice_clipped",))
        source += r'''
static void check(const char *text, int x, int clip_x, int clip_w, const char *visible, int draw_x)
{
    expected = visible;
    expected_len = strlen(visible);
    expected_x = draw_x;
    drawn = calls = 0;
    solar_os_gfx_t gfx = 0;
    doc_render_text_slice_clipped(&gfx, text, strlen(text), x, 12, 7, clip_x, clip_w);
    assert(drawn == expected_len);
}

int main(void)
{
    check("aö€їțZ", 0, 0, 42, "aö€їțZ", 0);
    check("aö€їțZ", 0, 7, 21, "ö€ї", 7);
    check("aö€їțZ", -7, 0, 21, "ö€ї", 0);
    check("aö€їțZ", 0, 42, 7, "", 42);
    check("aö€їțZ", 0, 0, 6, "", 0);
    check("𐍈öZ", 0, 7, 14, "öZ", 7);
    char text[401];
    for (size_t i = 0; i < 200; i++) memcpy(text + i * 2, "ö", 2);
    text[400] = '\0';
    check(text, 0, 0, 1400, text, 0);
    assert(calls > 1 && expected_x == 1400);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp, "doc_utf8.c")
            path.write_text(source)
            binary = Path(tmp, "doc-utf8")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                str(path), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
