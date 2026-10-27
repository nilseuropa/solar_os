#include "u8g2.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t u8g2_font_5x8_tf[];
static uint8_t pixels[240 * 64 / 8];
static const u8x8_display_info_t info = {
    .tile_width = 30, .tile_height = 8, .pixel_width = 240, .pixel_height = 64,
};
static uint8_t display(u8x8_t *u, uint8_t msg, uint8_t arg, void *ptr) {
    (void)arg; (void)ptr;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY)
        u8x8_d_helper_display_setup_memory(u, &info);
    return 1;
}
typedef struct { int cell_ascent, line_height, char_width; } solar_os_terminal_t;
static void terminal_set_draw_color(const solar_os_terminal_t *t, u8g2_t *u, uint8_t white) {
    (void)t; u8g2_SetDrawColor(u, white ? 0 : 1);
}
static bool terminal_draw_box_cell(u8g2_t *u, int x, int y, int w, int h, uint16_t c) {
    (void)u; (void)x; (void)y; (void)w; (void)h; (void)c; return false;
}
#define terminal_draw_block_cell terminal_draw_box_cell
static const uint8_t *terminal_selected_font(const solar_os_terminal_t *t, bool b, bool i) {
    (void)t; (void)b; (void)i; return u8g2_font_5x8_tf;
}
#include "terminal_cell.inc"
int main(int argc, char **argv) {
    u8g2_t u;
    u8g2_SetupDisplay(&u, display, u8x8_cad_empty, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u, pixels, 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&u);
    u8g2_SetFont(&u, u8g2_font_5x8_tf);
    u8g2_SetFontMode(&u, 1);
    u8g2_SetFontPosBaseline(&u);
    assert(u8g2_GetMaxCharHeight(&u) == 8 && u.font_info.y_offset == -1);
    /* Every printable ASCII/Latin-1 glyph must fit its 6x8 cell, including
     * descenders and accents. All eight terminal rows must be identical. */
    for (unsigned glyph = 33; glyph <= 255; glyph++) {
        if (glyph >= 127 && glyph <= 160) continue;
        u8g2_ClearBuffer(&u);
        assert(u8g2_GetGlyphWidth(&u, glyph) == 5);
        for (unsigned row = 0; row < 8; row++)
            u8g2_DrawGlyph(&u, 4, row * 8 + 7, glyph);
        bool ink = false;
        for (unsigned x = 0; x < 240; x++) {
            if (pixels[x]) ink = true;
            if (x < 4 || x >= 10) assert(!pixels[x]);
            for (unsigned row = 1; row < 8; row++)
                assert(pixels[row * 240 + x] == pixels[x]);
        }
        /* Soft hyphen is allowed to have no visible bitmap. */
        assert(ink || glyph == 173);
    }
    /* The actual terminal cell renderer must keep underline inside each row,
     * including the last row, with only one descent pixel available. */
    solar_os_terminal_t terminal = {.cell_ascent = 7, .line_height = 8, .char_width = 6};
    u8g2_ClearBuffer(&u);
    for (unsigned row = 0; row < 8; row++)
        terminal_draw_cell(&terminal, &u, 1, 4, row * 8 + 7, 'A', false, false, true, false);
    for (unsigned row = 0; row < 8; row++)
        for (unsigned x = 4; x < 10; x++) {
            assert(pixels[row * 240 + x] & 0x80);
            assert(pixels[row * 240 + x] == pixels[x]);
        }
    const char *lines[] = {
        "nils@waveshare:/ ls", "265 display.sh", "8785 hx20-de-solaros.json",
        "376 kg-expansion.toml", "287 custom-expansion.toml",
        "ABCDEF abcdef 0123456789", "[]{} () /\\ +-=_ :;.,!?", "gypqj  38 columns x 8 rows",
    };
    u8g2_ClearBuffer(&u);
    for (unsigned row = 0; row < 8; row++)
        for (unsigned col = 0; lines[row][col]; col++)
            u8g2_DrawGlyph(&u, 4 + col * 6, row * 8 + 7, (uint8_t)lines[row][col]);
    if (argc == 2) {
        FILE *f = fopen(argv[1], "wb");
        assert(f);
        fprintf(f, "P4\n240 64\n");
        for (unsigned y = 0; y < 64; y++)
            for (unsigned bx = 0; bx < 30; bx++) {
                uint8_t b = 0;
                for (unsigned bit = 0; bit < 8; bit++)
                    if (pixels[(y / 8) * 240 + bx * 8 + bit] & (1u << (y % 8)))
                        b |= (uint8_t)(0x80u >> bit);
                fputc(b, f);
            }
        fclose(f);
    }
    puts("6x8 bitmap ASCII/Latin-1 cell bounds and eight-row rendering passed");
    return 0;
}
