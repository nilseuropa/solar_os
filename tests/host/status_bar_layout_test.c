#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_terminal_internal.h"
#ifndef SOLAR_OS_TEST_REAL_STATUS_FONT
#define u8g2_font_solar_os_default_r_12_tf u8g2_font_5x8_tf
#endif
#include "solar_os_fonts.h"

#define TERM_STATUS_BAR_HEIGHT 16
#define TERM_STATUS_BAR_COMPACT_MAX_WIDTH 160
#define TERM_STATUS_BAR_ICON_GAP 4

static uint8_t pixels[400 * 2];
static int layout_x;
static unsigned labels;
static u8x8_display_info_t info = {.tile_height = 2, .pixel_height = 16};
static uint8_t display(u8x8_t *u, uint8_t msg, uint8_t arg, void *ptr)
{
    (void)arg; (void)ptr;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) u8x8_d_helper_display_setup_memory(u, &info);
    return 1;
}
static void terminal_set_draw_color(const solar_os_terminal_t *t, u8g2_t *u, uint8_t white)
{ (void)t; u8g2_SetDrawColor(u, white ? 0 : 1); }
static u8g2_uint_t track_string(u8g2_t *u, u8g2_uint_t x, u8g2_uint_t y, const char *text)
{
    if (strcmp(text, "US") == 0 || strcmp(text, "UA") == 0) {
        labels++; layout_x = (int)x;
        assert(x >= 4 && (int)x + (int)u8g2_GetStrWidth(u, text) < (int)info.pixel_width - 4);
    }
    return u8g2_DrawStr(u, x, y, text);
}
#define u8g2_DrawStr track_string
#include "status_bar_layout.inc"
#undef u8g2_DrawStr

int main(int argc, char **argv)
{
    const unsigned widths[] = {80, 128, 160, 176, 240, 400};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        info.pixel_width = widths[w]; info.tile_width = widths[w] / 8U;
        u8g2_t u;
        u8g2_SetupDisplay(&u, display, u8x8_cad_empty, u8x8_dummy_cb, u8x8_dummy_cb);
        u8g2_SetupBuffer(&u, pixels, 2, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
        u8g2_InitDisplay(&u);
        static solar_os_terminal_t terminal;
        terminal.status_bar_height = 16;
        terminal.status_bar = (solar_os_status_bar_t){
            .battery_valid = true, .battery_percent = 98, .keyboard_count = 1,
            .bluetooth_supported = true, .bluetooth_enabled = true, .wifi_started = true,
            .wifi_connected = true, .wifi_has_ip = true, .wifi_level = 4,
            .audio_enabled = true, .audio_volume = 80, .hour = 12, .minute = 34,
            .sd_mounted = true, .radio_attached = true, .link_running = true,
        };
        uint8_t us[sizeof(pixels)], frame[sizeof(pixels)];
        for (unsigned time = 0; time < 2; time++) for (unsigned inbox = 0; inbox < 2; inbox++) {
            terminal.status_bar.time_valid = time;
            terminal.status_bar.inbox_unread = inbox ? 123 : 0;
            for (unsigned layout = 0; layout < 2; layout++) {
                strcpy(terminal.status_bar.keyboard_layout, layout ? "UA" : "US");
                labels = 0;
                terminal_draw_status_bar(&terminal, &u);
                assert(labels == 1);
                memcpy(frame, pixels, sizeof(pixels));
                if (!layout) memcpy(us, pixels, sizeof(pixels));
                else assert(memcmp(us, pixels, widths[w] * 2U) != 0);
                const int label_width = (int)u8g2_GetStrWidth(&u, terminal.status_bar.keyboard_layout);
                u8g2_SetDrawColor(&u, 1);
                u8g2_DrawBox(&u, 0, 0, widths[w], 16);
                u8g2_SetDrawColor(&u, 0);
                u8g2_DrawStr(&u, layout_x, 13, terminal.status_bar.keyboard_layout);
                /* No icon, inbox count or clock may alter the label's pixels. */
                for (unsigned row = 0; row < 2; row++)
                    for (int x = layout_x; x < layout_x + label_width; x++)
                        assert(frame[row * widths[w] + x] == pixels[row * widths[w] + x]);
                if (argc == 2 && time && inbox) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%u-%s.pbm", argv[1], widths[w], terminal.status_bar.keyboard_layout);
                    FILE *f = fopen(path, "wb"); assert(f);
                    fprintf(f, "P1\n%u 16\n", widths[w]);
                    for (unsigned y = 0; y < 16; y++) {
                        for (unsigned x = 0; x < widths[w]; x++)
                            fprintf(f, "%u ", (frame[(y / 8) * widths[w] + x] >> (y % 8)) & 1U);
                        fputc('\n', f);
                    }
                    fclose(f);
                }
            }
        }
        solar_os_status_bar_t other = terminal.status_bar;
        strcpy(other.keyboard_layout, "US");
        assert(!terminal_status_bar_equal(&terminal.status_bar, &other));
        const uint32_t before = terminal_render_status_hash(&terminal);
        terminal.status_bar = other;
        assert(before != terminal_render_status_hash(&terminal));
    }
    puts("US/UA status text, invalidation and icon spacing passed at widths 80..400");
    return 0;
}
