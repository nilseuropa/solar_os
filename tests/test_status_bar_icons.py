from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/services/solar_os_terminal.c").read_text()


def function(name):
    start = SOURCE.rfind("\nstatic ", 0, SOURCE.index(name + "(")) + 1
    opening = SOURCE.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


class StatusBarIconsTest(unittest.TestCase):
    def test_layout_text_fits_with_icons_and_clock(self):
        component = ROOT / "components/u8g2"
        cmake = (component / "CMakeLists.txt").read_text().split("\n)", 1)[0]
        sources = [str(component / name) for name in re.findall(r'"([^"]+\.c)"', cmake)]
        font = ROOT / "fonts/build/u8g2/u8g2_font_solar_os_default_r_12_tf.c"
        flags = ["-DSOLAR_OS_TEST_REAL_STATUS_FONT", str(font)] if font.exists() else []
        with tempfile.TemporaryDirectory() as directory:
            icons = SOURCE[SOURCE.index("static void terminal_draw_status_slash("):
                           SOURCE.index("static void terminal_draw_footer(")]
            checks = production_functions(SOURCE, (
                "terminal_status_bar_equal", "terminal_render_hash_bytes",
                "terminal_render_hash_value", "terminal_render_status_hash",
            ))
            Path(directory, "status_bar_layout.inc").write_text(icons + checks)
            binary = Path(directory) / "status-layout"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(component / "src/clib"), "-I" + directory,
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/status_bar_layout_test.c"), *sources, *flags,
                "-Wl,--gc-sections", "-o", str(binary),
            ], check=True)
            preview = os.environ.get("SOLAROS_LAYOUT_PREVIEW")
            if preview:
                Path(preview).mkdir(parents=True, exist_ok=True)
            subprocess.run([str(binary), *([preview] if preview else [])], check=True)

    def test_production_icon_pixels(self):
        helpers = "\n".join(function(name) for name in (
            "terminal_draw_status_slash", "terminal_draw_diag_down",
            "terminal_draw_diag_up", "terminal_draw_keyboard_icon",
            "terminal_draw_bluetooth_icon",
        ))
        harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef unsigned u8g2_uint_t;
typedef struct { unsigned char pixels[16][24]; } u8g2_t;
static void solar_os_canvas_pixel(u8g2_t *u, unsigned x, unsigned y) {
    assert(x < 24 && y < 16); u->pixels[y][x] = 1;
}
static void solar_os_canvas_hline(u8g2_t *u, unsigned x, unsigned y, unsigned n) {
    for (unsigned i=0; i<n; ++i) solar_os_canvas_pixel(u,x+i,y);
}
static void solar_os_canvas_vline(u8g2_t *u, unsigned x, unsigned y, unsigned n) {
    for (unsigned i=0; i<n; ++i) solar_os_canvas_pixel(u,x,y+i);
}
static void solar_os_canvas_frame(u8g2_t *u, unsigned x, unsigned y, unsigned w, unsigned h) {
    solar_os_canvas_hline(u,x,y,w); solar_os_canvas_hline(u,x,y+h-1,w);
    solar_os_canvas_vline(u,x,y,h); solar_os_canvas_vline(u,x+w-1,y,h);
}
static void solar_os_canvas_box(u8g2_t *u, unsigned x, unsigned y, unsigned w, unsigned h) {
    for (unsigned i=0; i<h; ++i) solar_os_canvas_hline(u,x,y+i,w);
}
''' + helpers + r'''
int main(void) {
    u8g2_t icons[5] = {0}, disabled_scanning = {0};
    terminal_draw_bluetooth_icon(&icons[0],0,3,true,false);
    terminal_draw_bluetooth_icon(&icons[1],0,3,false,false);
    terminal_draw_bluetooth_icon(&icons[2],0,3,true,true);
    terminal_draw_keyboard_icon(&icons[3],0,3,true);
    terminal_draw_keyboard_icon(&icons[4],0,3,false);
    terminal_draw_bluetooth_icon(&disabled_scanning,0,3,false,true);
    assert(memcmp(&icons[1],&disabled_scanning,sizeof(u8g2_t)) == 0);
    for (int a=0; a<5; ++a) for (int b=a+1; b<5; ++b)
        assert(memcmp(&icons[a],&icons[b],sizeof(u8g2_t)) != 0);
    for (int a=0; a<5; ++a) for (int y=0; y<16; ++y)
        for (int x=a<3 ? 14 : 18; x<24; ++x) assert(!icons[a].pixels[y][x]);
    puts("P1\n120 16");
    for (int y=0; y<16; ++y) {
        for (int a=0; a<5; ++a) for (int x=0; x<24; ++x)
            printf("%u ",icons[a].pixels[y][x]);
        puts("");
    }
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "icons.c"
            binary = Path(directory) / "icons"
            source.write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            str(source), "-o", str(binary)], check=True)
            preview = subprocess.check_output([str(binary)])
            if os.environ.get("SOLAROS_ICON_PREVIEW"):
                Path(os.environ["SOLAROS_ICON_PREVIEW"]).write_bytes(preview)

    def test_status_changes_invalidate_rendering(self):
        for field in ("bluetooth_supported", "bluetooth_enabled", "bluetooth_scanning"):
            self.assertIn("a->" + field + " == b->" + field,
                          function("terminal_status_bar_equal"))
            self.assertIn("status->" + field, function("terminal_render_status_hash"))

    def test_keyboard_and_bluetooth_are_independent(self):
        draw = function("terminal_draw_status_bar")
        self.assertEqual(draw.count("status->keyboard_count > 0"), 2)
        self.assertEqual(draw.count("terminal_draw_bluetooth_icon("), 2)
        self.assertNotIn("scanning", function("terminal_draw_keyboard_icon"))
        self.assertNotIn("count", function("terminal_draw_keyboard_icon"))
        for path in ("src/main.c", "src/shell/solar_os_shell_hardware.c"):
            source = (ROOT / path).read_text()
            self.assertNotIn("keyboard_scanning", source)
            self.assertIn("status.bluetooth_enabled = solar_os_ble_keyboard_enabled_for_current_boot()", source)
            self.assertIn("status.bluetooth_scanning =", source)


if __name__ == "__main__":
    unittest.main()
