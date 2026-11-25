#include "solar_os_gfx_widgets.h"
#include <string.h>
#include "solar_os_native_client_ui.h"

void solar_os_gfx_label(solar_os_gfx_t *gfx, int x, int baseline, int width,
                         const char *text)
{
    if (!text || width <= 0) return;
    const size_t length = solar_os_gfx_text_fit(gfx, text, strlen(text), (size_t)width, NULL);
    if (length) solar_os_gfx_text_span(gfx, x, baseline, text, length);
}

void solar_os_gfx_centered_label(solar_os_gfx_t *gfx, int x, int baseline, int width,
                                  const char *text)
{
    if (!text || width <= 0) return;
    size_t advance;
    const size_t length = solar_os_gfx_text_fit(gfx, text, strlen(text), (size_t)width, &advance);
    if (length) solar_os_gfx_text_span(gfx, x + (width - (int)advance) / 2, baseline, text, length);
}

void solar_os_gfx_input(solar_os_gfx_t *gfx, int x, int baseline, int width,
                         const char *text, solar_os_ui_input_state_t *state)
{
    if (!gfx || !text || !state || width <= 0) return;
    solar_os_ui_input_reconcile(text, state, SIZE_MAX);
    const size_t length = strlen(text);
    size_t cursor_width = solar_os_gfx_text_advance(gfx, "_", 1);
    if (!cursor_width) cursor_width = 1;
    if (cursor_width > (size_t)width) cursor_width = (size_t)width;
    size_t cursor_advance;
    state->view = solar_os_gfx_text_tail(gfx, text, state->cursor,
        (size_t)width - cursor_width, &cursor_advance);
    const char *visible = text + state->view;
    const size_t fitted = solar_os_gfx_text_fit(gfx, visible, length - state->view, (size_t)width, NULL);
    if (fitted) solar_os_gfx_text_span(gfx, x, baseline, visible, fitted);
    solar_os_gfx_fill_rect(gfx, x + (int)cursor_advance, baseline + 1, (int)cursor_width, 2);
}

void solar_os_gfx_dialog(solar_os_gfx_t *gfx, int width, int height,
                          const char *title, const char *text,
                          solar_os_ui_input_state_t *state, const char *help)
{
    if (!gfx || width < 16 || height < 16) return;
    const bool compact = height < 160 || width < 240;
    const int w = width - (compact ? 4 : 24);
    int h = compact ? height - 4 : 76;
    if (compact && h > 60) h = 60;
    const int x = (width - w) / 2, y = (height - h) / 2;
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_fill_rect(gfx, x, y, w, h);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_rect(gfx, x, y, w, h);
    if (h >= 28) {
        solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_BOLD_14);
        solar_os_gfx_label(gfx, x + 4, y + (compact ? 10 : 20), w - 8, title);
    }
    const int baseline = h < 28 ? y + (h + 7) / 2 : y + h / 2 + 4;
    solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_MONO_12);
    /* Draw the cursor last so following title/help rendering cannot cover it. */
    if (h >= 28) {
        solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_label(gfx, x + 4, y + h - 5, w - 8, help);
        solar_os_gfx_set_font(gfx, compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_MONO_12);
    }
    solar_os_gfx_input(gfx, x + 4, baseline, w - 8, text ? text : "", state);
}
