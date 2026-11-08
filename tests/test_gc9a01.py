from pathlib import Path
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class Gc9a01Test(unittest.TestCase):
    def test_initialization_order_timing_and_transport_errors(self):
        source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define TAG "test"
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_RETURN_ON_ERROR(expr, ...) do { esp_err_t e = (expr); if (e) return e; } while (0)
typedef struct {
    struct { bool gc9a01, st7796, st7789, invert_color; uint8_t madctl; } config;
    uint16_t background_rgb565;
    esp_err_t last_error;
    int display_info;
} tft_ili9341_t;
typedef struct { int unused; } u8x8_t, u8x8_tile_t;
enum { U8X8_MSG_DISPLAY_SETUP_MEMORY, U8X8_MSG_DISPLAY_INIT,
       U8X8_MSG_DISPLAY_SET_POWER_SAVE, U8X8_MSG_DISPLAY_DRAW_TILE };
static tft_ili9341_t *active_display;
static struct { uint8_t command, data[16]; size_t length; unsigned time; } sent[128];
static unsigned count, elapsed, fail_at, resets, clears, enables;
static esp_err_t clear_error;
static bool ili9341_checked_cmd_data(tft_ili9341_t *display, uint8_t command,
    const uint8_t *data, size_t length)
{
    assert(count < 128 && length <= sizeof(sent[0].data));
    sent[count].command = command;
    sent[count].length = length;
    sent[count].time = elapsed;
    if (length) memcpy(sent[count].data, data, length);
    count++;
    if (count == fail_at) { display->last_error = ESP_FAIL; return false; }
    return true;
}
static bool ili9341_checked_cmd(tft_ili9341_t *display, uint8_t command)
{ return ili9341_checked_cmd_data(display, command, NULL, 0); }
static void vTaskDelay(unsigned ticks) { elapsed += ticks; }
static void ili9341_hardware_reset(tft_ili9341_t *display) { (void)display; resets++; }
static esp_err_t ili9341_fill_screen(tft_ili9341_t *display, uint16_t color)
{
    (void)display;
    assert(color == 0xffff);
    assert(sent[count - 1].command == 0x11 && elapsed - sent[count - 1].time >= 120);
    clears++;
    return clear_error;
}
static void ili9341_set_backlight_power(tft_ili9341_t *display, bool enabled)
{ (void)display; if (enabled) enables++; }
static void ili9341_invalidate_shadow(tft_ili9341_t *display) { (void)display; }
static void u8x8_d_helper_display_setup_memory(u8x8_t *display, const int *info)
{ (void)display; (void)info; }
static esp_err_t ili9341_cmd(tft_ili9341_t *display, uint8_t command)
{ return ili9341_checked_cmd(display, command) ? ESP_OK : ESP_FAIL; }
static esp_err_t ili9341_draw_tile(tft_ili9341_t *display, const u8x8_tile_t *tile)
{ (void)display; (void)tile; return ESP_OK; }
'''
        source += production_functions(
            (ROOT / "src/drivers/tft_ili9341.c").read_text(),
            ("solar_os_gc9a01_init_registers", "ili9341_full_init", "ili9341_u8x8_display_cb"))
        source += r'''
static int command_index(uint8_t command) {
    for (unsigned i = 0; i < count; i++) if (sent[i].command == command) return (int)i;
    return -1;
}
static void reset(void) {
    memset(sent, 0, sizeof(sent));
    count = elapsed = fail_at = resets = clears = enables = 0;
    clear_error = ESP_OK;
}
int main(void) {
    tft_ili9341_t display = {.config = {.gc9a01 = true, .madctl = 0x08,
        .invert_color = true}, .background_rgb565 = 0xffff};
    active_display = &display;
    assert(ili9341_u8x8_display_cb(NULL, U8X8_MSG_DISPLAY_INIT, 0, NULL) == 1);
    assert(display.last_error == ESP_OK && resets == 1 && clears == 1 && enables == 1);
    assert(sent[0].command == 0x01 && sent[1].time >= 120);
    assert(sent[1].command == 0xef && sent[2].command == 0xeb && sent[2].data[0] == 0x14);
    assert(sent[3].command == 0xfe && sent[4].command == 0xef);
    int pixel = command_index(0x3a), access = command_index(0x36);
    assert(pixel > 0 && sent[pixel].length == 1 && sent[pixel].data[0] == 0x05);
    assert(access > 0 && sent[access].length == 1 && sent[access].data[0] == 0x08);
    assert(command_index(0x21) > pixel && command_index(0x20) == -1);
    assert(command_index(0x11) > command_index(0x21));
    assert(sent[count - 1].command == 0x29 && elapsed - sent[count - 1].time >= 20);
    assert(command_index(0xcf) == -1 && command_index(0xb2) == -1); /* Other controllers. */
    const unsigned total = count;
    for (unsigned failure = 1; failure <= total; failure++) {
        reset(); fail_at = failure; display.last_error = ESP_OK;
        assert(ili9341_u8x8_display_cb(NULL, U8X8_MSG_DISPLAY_INIT, 0, NULL) == 0);
        assert(display.last_error == ESP_FAIL && count == failure && enables == 0);
    }
    reset(); clear_error = ESP_ERR_NO_MEM; display.last_error = ESP_OK;
    assert(ili9341_u8x8_display_cb(NULL, U8X8_MSG_DISPLAY_INIT, 0, NULL) == 0);
    assert(display.last_error == clear_error && command_index(0x29) == -1 && enables == 0);
    reset(); display.config.invert_color = false; display.config.madctl = 0;
    assert(ili9341_u8x8_display_cb(NULL, U8X8_MSG_DISPLAY_INIT, 0, NULL) == 1);
    assert(command_index(0x21) == -1 && command_index(0x20) > pixel);
    assert(sent[command_index(0x36)].data[0] == 0);
}
'''
        run_c(source)


if __name__ == "__main__":
    unittest.main()
