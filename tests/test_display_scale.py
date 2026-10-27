from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayScaleTest(unittest.TestCase):
    def test_resize_preserves_output_cursor_and_history(self):
        source = r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_terminal_internal.h"
static size_t requested_rows, requested_cols, pushed;
static size_t terminal_rows(const solar_os_terminal_t *t) {return t->rows;}
static void scrollback_push(solar_os_terminal_t *t, const solar_os_terminal_cell_t *line,
    const uint32_t *bold, const uint32_t *italic, const uint32_t *underline, const uint32_t *inverse) {
    (void)t;(void)bold;(void)italic;(void)underline;(void)inverse;
    assert(line[0]=='A'+pushed);pushed++;
}
static void terminal_apply_settings(solar_os_terminal_t *t, bool clear) {
    assert(!clear);t->rows=requested_rows;t->cols=requested_cols;
    if(t->cursor_row>=t->rows)t->cursor_row=t->rows-1;
    if(t->cursor_col>t->cols)t->cursor_col=t->cols;
}
void solar_os_terminal_clear_line_from(solar_os_terminal_t *t, size_t row, size_t col) {
    assert(col==0);memset(t->lines[row],0,sizeof(t->lines[row]));
}
'''
        source += production_functions((ROOT / "src/services/solar_os_terminal.c").read_text(),
                                       ("scroll_up", "terminal_apply_scale_geometry"))
        source += r'''
int main(void) {
    solar_os_terminal_t *t=calloc(1,sizeof(*t));
    t->rows=10;t->cols=20;t->cursor_row=8;t->cursor_col=12;
    for(unsigned row=0;row<10;row++){t->lines[row][0]='A'+row;t->bold[row][0]=row+1;}
    requested_rows=4;requested_cols=8;terminal_apply_scale_geometry(t);
    assert(pushed==5 && t->cursor_row==3 && t->cursor_col==8);
    for(unsigned row=0;row<4;row++){
        assert(t->lines[row][0]=='F'+row && t->bold[row][0]==row+6);
    }
    requested_rows=8;terminal_apply_scale_geometry(t);
    assert(pushed==5 && t->cursor_row==3 && t->cursor_col==8);
    for(unsigned row=4;row<8;row++)assert(t->lines[row][0]==0);
    free(t);return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path, binary = Path(tmp, "resize.c"), Path(tmp, "resize")
            path.write_text(source)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
                "-I", str(ROOT / "components/u8g2/src/clib"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_integer_scaling_with_real_u8g2(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "scale")
            sources = [ROOT / "src/services/solar_os_display_scale.c",
                       ROOT / "tests/host/display_scale_test.c"]
            sources += [ROOT / "components/u8g2/src/clib" / (name + ".c") for name in (
                "u8g2_setup", "u8g2_buffer", "u8g2_font", "u8g2_ll_hvline", "u8g2_hvline",
                "u8g2_intersection", "u8x8_setup", "u8x8_display")]
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"),
                "-idirafter", str(ROOT / "include"),
                "-I", str(ROOT / "components/u8g2/src/clib"), *map(str, sources),
                "-Wl,--gc-sections", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
