#include "solar_os_native_ui.h"
#include "solar_os_native_ui_abi.h"
#include "solar_os_native_client_ui.h"
#include "solar_os_tui_widgets.h"

static void ui_widgets(solar_os_gfx_t *gfx, u8g2_t *u)
{
    const solar_os_native_ui_api_v1_t *ui = solar_os_native_ui_get_service("ui", 1, 8);
    assert(ui && ui->struct_size == sizeof(*ui));
    assert(!solar_os_native_ui_get_service("ui", 2, 8));
    assert(!solar_os_native_ui_get_service("ui", 1, sizeof(*ui) + 1));
    assert(!solar_os_native_ui_get_service("other", 1, 8));
    assert(ui->input_key == solar_os_tui_input_key && ui->input_text == solar_os_tui_input_text);
    assert(ui->viewport_key == solar_os_tui_viewport_key && ui->viewport_reconcile == solar_os_tui_viewport_reconcile);
    const unsigned before = allocations;
    solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_MONO_14);
    const size_t step = solar_os_gfx_text_advance(gfx, "A", 1);
    char text[32] = "abcdefghij";
    solar_os_ui_input_state_t state = {strlen(text), 0};
    const solar_os_display_rect_t canvas = solar_os_display_canvas_rect(u);
    for (size_t cursor = 0; cursor <= strlen(text); cursor++) {
        state.cursor = cursor;
        solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        ui->input(gfx, 10, 30, (int)(3 * step), text, &state);
        assert(state.view == (cursor > 2 ? cursor - 2 : 0));
        const int caret = 10 + (int)((cursor - state.view) * step);
        assert(mono_pixel(u, canvas.x + caret, canvas.y + 31));
        assert(mono_pixel(u, canvas.x + caret + step - 1, canvas.y + 32));
        assert(!mono_pixel(u, canvas.x + caret + step, canvas.y + 32));
    }
    strcpy(text, "AB\xc3\xb6" "CD"); state.cursor = 4;
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    ui->input(gfx, 10, 30, (int)(3 * step), text, &state);
    assert(state.view == 1);
    ui->input_key(text, sizeof(text), &state, SOLAR_OS_KEY_LEFT, SIZE_MAX);
    assert(state.cursor == 2);
    assert(ui->input_text(text, sizeof(text), &state, "\xc3\xa9", 2, SIZE_MAX) == SOLAR_OS_UI_INPUT_CHANGED);
    solar_os_tui_viewport_t view = {0, SIZE_MAX};
    assert(ui->viewport_key(&view, SOLAR_OS_KEY_UP, 5, 2, true) && view.cursor == 4 && view.top == 3);
    ui->viewport_reconcile(&view, 1, 2); assert(!view.cursor && !view.top);
    assert(!strcmp(text, "AB\xc3\xa9\xc3\xb6" "CD"));
    ui->input(gfx, 10, 30, (int)(3 * step), text, &state);
    assert(state.view == 1 && state.cursor == 4);
    /* Labels truncate at the same glyph boundary as the field renderer. */
    uint8_t expected[52 * 52 * 8];
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    solar_os_gfx_text_span(gfx, 10, 30, "ABignored", 2);
    memcpy(expected, u->tile_buf_ptr, sizeof(expected));
    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    ui->label(gfx, 10, 30, (int)(2 * step), "ABignored");
    assert(!memcmp(expected, u->tile_buf_ptr, sizeof(expected)));
    const int sizes[][2] = {{16,16}, {80,32}, {128,64}, {240,135}, {291,291}};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(*sizes); i++) {
        state.cursor = strlen(text);
        ui->dialog(gfx, sizes[i][0], sizes[i][1], "Title", text, &state, "Help");
        assert(state.cursor == strlen(text) && state.view <= state.cursor);
    }
    assert(allocations == before);
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
}
