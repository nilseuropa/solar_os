from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c


ROOT = Path(__file__).resolve().parents[1]


class DisplayLayoutPresentTest(unittest.TestCase):
    def test_layout_changes_follow_the_session_owner_and_allow_cleanup_retry(self):
        source = (ROOT / "src/services/solar_os_sessions.c").read_text()
        records = [re.search(r"^typedef struct \{[^}]*\} " + name + ";", source, re.M).group()
                   for name in ("session_layout_create_t", "session_layout_remove_t")]
        functions = production_functions(source, (
            "session_builtin_shell_on_target", "session_layout_create_on_owner",
            "solar_os_sessions_create_display_layout", "session_layout_remove_on_owner",
            "solar_os_sessions_remove_display_layout",
        ))
        fixture = (ROOT / "tests/host/display_layout_sessions_test.inc").read_text()
        run_c(fixture.replace("/* FUNCTIONS */", "\n".join(records) + "\n" + functions),
              "-fsanitize=address,undefined", "-no-pie")

    def test_streaming_frames_with_real_u8g2(self):
        source = (ROOT / "src/services/solar_os_display_layout.c").read_text()
        types = source[source.index("typedef struct solar_os_display_layout_runtime"):
                       source.index('static const char *TAG')]
        functions = production_functions(source, (
            "layout_name_valid", "layout_submit", "layout_display_cb",
            "layout_present_frame", "layout_prepare_logical"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "layout_frames.inc").write_text(types + functions)
            sources = [ROOT / "tests/host/display_layout_frame_test.c",
                       ROOT / "src/services/solar_os_display_layout_core.c"]
            sources += [ROOT / "components/u8g2/src/clib" / (name + ".c") for name in (
                "u8g2_setup", "u8g2_buffer", "u8g2_font", "u8g2_ll_hvline", "u8g2_hvline",
                "u8g2_intersection", "u8g2_box", "u8x8_setup", "u8x8_display", "u8x8_cad", "u8x8_byte")]
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-parameter", "-Wno-sign-compare", "-ffunction-sections", "-fdata-sections",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                "-I" + str(ROOT / "components/u8g2/src/clib"),
                *map(str, sources), "-Wl,--gc-sections", "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)

    def test_bulk_copy_and_coordinate_fallback(self):
        source = (ROOT / "src/services/solar_os_display_layout.c").read_text()
        function = production_functions(source, ("layout_present_backing",))
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_display_layout_core.h"
typedef int esp_err_t;
enum {ESP_OK, ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_STATE, SOLAR_OS_DISPLAY_PRESENT_GRAPHICS};
static const char normal_rotation, other_rotation;
#define U8G2_R0 (&normal_rotation)
typedef struct {
    const void *cb;
    unsigned pixel_buf_width, tile_buf_height, draw_color;
    uint16_t width, height;
    uint8_t *buffer;
} u8g2_t;
#define u8g2_GetBufferTileHeight(canvas) ((canvas)->tile_buf_height)
#define u8g2_GetBufferPtr(canvas) ((canvas)->buffer)
typedef struct {uint16_t width, height; uint8_t *presenting;} solar_os_display_layout_runtime_t;
typedef struct {char name[16]; solar_os_display_layout_rect_t region;} display_layout_backing_t;
typedef struct {bool ready; uint16_t width, height; u8g2_t *u8g2;} solar_os_display_target_t;
static solar_os_display_target_t target;
static bool available = true;
static unsigned boxes, pixels, presents;
static bool solar_os_display_find_target(const char *name, solar_os_display_target_t *out)
{assert(!strcmp(name, "display0")); *out = target; return available;}
static void u8g2_SetDrawColor(u8g2_t *canvas, unsigned color) {canvas->draw_color = color;}
static void u8g2_DrawBox(u8g2_t *canvas, unsigned x, unsigned y, unsigned width, unsigned height)
{
    boxes++;
    for (unsigned row = y; row < y + height; row++)
        for (unsigned col = x; col < x + width; col++)
            solar_os_display_layout_mono_set(canvas->buffer, canvas->width, canvas->height,
                col, row, canvas->draw_color != 0);
}
static void u8g2_DrawPixel(u8g2_t *canvas, unsigned x, unsigned y)
{
    pixels++;
    solar_os_display_layout_mono_set(canvas->buffer, canvas->width, canvas->height,
        x, y, canvas->draw_color != 0);
}
static void solar_os_display_present(u8g2_t *canvas, int mode)
{assert(canvas == target.u8g2 && canvas->draw_color == 1 && mode == SOLAR_OS_DISPLAY_PRESENT_GRAPHICS); presents++;}
/* PRESENT */
int main(void)
{
    uint8_t source[152 * 8], framebuffer[104 * 4], bulk[sizeof(framebuffer)];
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 71U + 43U);
    memset(framebuffer, 0xa5, sizeof(framebuffer));
    solar_os_display_layout_runtime_t layout = {.width = 147, .height = 61, .presenting = source};
    display_layout_backing_t backing = {.name = "display0", .region = {13, 7, 101, 27}};
    u8g2_t canvas = {.cb = U8G2_R0, .pixel_buf_width = 104, .tile_buf_height = 4,
        .width = 101, .height = 27, .buffer = framebuffer};
    target = (solar_os_display_target_t){true, 101, 27, &canvas};
    assert(layout_present_backing(&layout, &backing) == ESP_OK);
    assert(presents == 1 && boxes == 0 && pixels == 0);
    memcpy(bulk, framebuffer, sizeof(bulk));

    /* Rotated targets must keep using u8g2's coordinate-aware drawing API. */
    memset(framebuffer, 0xa5, sizeof(framebuffer)); canvas.cb = &other_rotation;
    assert(layout_present_backing(&layout, &backing) == ESP_OK);
    assert(presents == 2 && boxes == 1 && pixels > 0);
    assert(!memcmp(framebuffer, bulk, sizeof(bulk)));

    canvas.cb = U8G2_R0; canvas.tile_buf_height = 1;
    assert(layout_present_backing(&layout, &backing) == ESP_OK && boxes == 2);
    canvas.tile_buf_height = 4; canvas.pixel_buf_width = 112;
    assert(layout_present_backing(&layout, &backing) == ESP_OK && boxes == 3);
    available = false;
    assert(layout_present_backing(&layout, &backing) == ESP_ERR_NOT_FOUND);
    available = true; target.ready = false;
    assert(layout_present_backing(&layout, &backing) == ESP_ERR_INVALID_STATE);
    target.ready = true; target.width--;
    assert(layout_present_backing(&layout, &backing) == ESP_ERR_INVALID_STATE);
    assert(presents == 4);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(harness.replace("/* PRESENT */", function))
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I" + str(ROOT / "src/services"), "-idirafter", str(ROOT / "include"), str(path / "test.c"),
                str(ROOT / "src/services/solar_os_display_layout_core.c"),
                "-o", str(path / "test"),
            ], check=True)
            subprocess.run([str(path / "test")], check=True)


if __name__ == "__main__":
    unittest.main()
