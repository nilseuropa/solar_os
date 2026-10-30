#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"

#include "controls.inc"
enum { SOLAR_OS_GFX_COLOR_WHITE, SOLAR_OS_GFX_COLOR_BLACK };
enum {
    SOLAR_OS_GFX_FONT_BITMAP_8, SOLAR_OS_GFX_FONT_MONO_12,
    SOLAR_OS_GFX_FONT_BOLD_12, SOLAR_OS_GFX_FONT_BOLD_14, SOLAR_OS_GFX_FONT_BOLD_20,
};
enum { SOLAR_OS_TUI_ATTR_NORMAL, SOLAR_OS_TUI_ATTR_BOLD, SOLAR_OS_TUI_ATTR_INVERSE };
typedef struct { int width, height, font; } solar_os_gfx_t;
typedef struct { size_t rows, cols; } solar_os_tui_t;
typedef struct { size_t cursor, top; } solar_os_tui_viewport_t;
typedef struct { solar_os_gfx_t *gfx; } solar_os_context_t;
typedef struct { bool running; char playback_stream[48]; uint32_t sample_rate; } solar_os_synth_status_t;
static struct {
    funcgen_control_t selected;
    solar_os_tui_viewport_t controls;
    solar_os_tui_t tui;
    bool suspended, enabled, redraw;
    esp_err_t last_error;
    void *scope;
} funcgen;

typedef struct { int x, y, w, h; } box_t;
static box_t text_boxes[32], scope_box;
static size_t texts, scopes, circles;
static bool selected_label_drawn, selected_value_drawn;
static char screen[32][100];
static FILE *trace;
static const char *const values[] = {
    "Triangle", "8000 Hz", "100%", "99%", "On", "8000 Hz", "60.0 s",
    "runtime-output-with-a-very-long-stream-name",
};

size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    size_t length = strlen(src);
    if (capacity) { size_t n = length < capacity - 1 ? length : capacity - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "ESP_ERR_INVALID_STATE_WITH_LONG_DETAIL"; }
static void funcgen_control_value(funcgen_control_t control, char *out, size_t size)
{ strlcpy(out, values[control], size); }
static unsigned funcgen_control_position(funcgen_control_t control) { (void)control; return 5; }
static const char *funcgen_output_label(void) { return values[FUNCGEN_CONTROL_OUTPUT]; }
static const char *funcgen_control_label(funcgen_control_t control);
static solar_os_gfx_t *solar_os_context_gfx(solar_os_context_t *ctx) { return ctx->gfx; }
static size_t solar_os_gfx_width(solar_os_gfx_t *gfx) { return (size_t)gfx->width; }
static size_t solar_os_gfx_height(solar_os_gfx_t *gfx) { return (size_t)gfx->height; }
static void solar_os_gfx_clear(solar_os_gfx_t *gfx, int color)
{
    (void)gfx; assert(color == SOLAR_OS_GFX_COLOR_WHITE); texts = scopes = circles = 0;
    selected_label_drawn = selected_value_drawn = false;
}
static void solar_os_gfx_set_color(solar_os_gfx_t *gfx, int color) { (void)gfx; assert(color == SOLAR_OS_GFX_COLOR_BLACK); }
static void solar_os_gfx_set_font(solar_os_gfx_t *gfx, int font) { gfx->font = font; }
static int font_width(int font) { return font == 0 ? 5 : (font == 4 ? 12 : (font == 3 ? 9 : 8)); }
static size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx, const char *text) { return strlen(text) * (size_t)font_width(gfx->font); }
static bool overlap(box_t a, box_t b)
{ return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h; }
static void bounds(solar_os_gfx_t *gfx, box_t box)
{ assert(box.x >= 0 && box.y >= 0 && box.x + box.w <= gfx->width && box.y + box.h <= gfx->height); }
static bool fitted_text_matches(const char *text, const char *expected)
{
    const size_t length = strlen(text);
    return !strcmp(text, expected) ||
        (length > 3 && !strcmp(text + length - 3, "...") && !strncmp(text, expected, length - 3));
}
static void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int y, const char *text)
{
    const int ascent[] = {7, 10, 10, 12, 17}, descent[] = {1, 2, 2, 2, 3};
    box_t box = {x, y - ascent[gfx->font], (int)solar_os_gfx_text_width(gfx, text), ascent[gfx->font] + descent[gfx->font]};
    bounds(gfx, box);
    for (size_t i = 0; i < texts; i++) assert(!overlap(box, text_boxes[i]));
    if (scopes) assert(!overlap(box, scope_box));
    assert(texts < 32); text_boxes[texts++] = box;
    selected_label_drawn |= fitted_text_matches(text, funcgen_control_label(funcgen.selected));
    selected_value_drawn |= fitted_text_matches(text, values[funcgen.selected]);
    if (trace) fprintf(trace, "TEXT %d %d %d %s\n", x, y, gfx->font, text);
}
static void solar_os_gfx_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    bounds(gfx, (box_t){x - radius, y - radius, radius * 2 + 1, radius * 2 + 1});
    circles++;
    if (trace) fprintf(trace, "CIRCLE %d %d %d\n", x, y, radius);
}
static void solar_os_gfx_line(solar_os_gfx_t *gfx, int x, int y, int x1, int y1)
{
    bounds(gfx, (box_t){x, y, 1, 1}); bounds(gfx, (box_t){x1, y1, 1, 1});
    if (trace) fprintf(trace, "LINE %d %d %d %d\n", x, y, x1, y1);
}
static void solar_os_oscilloscope_widget_draw(void *scope, solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    (void)scope; scope_box = (box_t){x, y, width, height}; bounds(gfx, scope_box); scopes++;
    for (size_t i = 0; i < texts; i++) assert(!overlap(scope_box, text_boxes[i]));
    if (trace) fprintf(trace, "SCOPE %d %d %d %d\n", x, y, width, height);
}
static void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }
static size_t solar_os_tui_rows(solar_os_tui_t *tui) { return tui->rows; }
static size_t solar_os_tui_cols(solar_os_tui_t *tui) { return tui->cols; }
static void solar_os_tui_clear(solar_os_tui_t *tui) { (void)tui; memset(screen, 0, sizeof(screen)); }
static esp_err_t solar_os_tui_write_cell(solar_os_tui_t *tui, size_t row, size_t col, size_t width, const char *text, unsigned attr)
{
    (void)attr; assert(row < tui->rows && col + width <= tui->cols && tui->rows <= 32 && tui->cols < 100);
    size_t length = strlen(text); if (length > width) length = width;
    memset(screen[row] + col, ' ', width); memcpy(screen[row] + col, text, length);
    return ESP_OK;
}
static void solar_os_tui_draw_title(solar_os_tui_t *tui, const char *title, const char *detail)
{
    solar_os_tui_write_cell(tui, 0, 0, tui->cols, title, 0);
    size_t n = strlen(detail); if (n > tui->cols) n = tui->cols;
    solar_os_tui_write_cell(tui, 0, tui->cols - n, n, detail, 0);
}
static void solar_os_synth_get_status(solar_os_synth_status_t *status)
{ *status = (solar_os_synth_status_t){.running = true, .sample_rate = 48000}; strlcpy(status->playback_stream, values[7], sizeof(status->playback_stream)); }
static void solar_os_tui_refresh(solar_os_tui_t *tui) { (void)tui; }

#include "funcgen_layout.inc"

int main(int argc, char **argv)
{
    const int sizes[][2] = {{400, 300}, {384, 240}, {384, 168}, {320, 240}, {240, 135}, {128, 128}, {128, 64}, {96, 48}, {64, 32}};
    for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        solar_os_gfx_t gfx = {.width = sizes[size][0], .height = sizes[size][1]};
        solar_os_context_t context = {.gfx = &gfx};
        for (unsigned error = 0; error < 2; error++) for (unsigned selected = 0; selected < FUNCGEN_CONTROL_COUNT; selected++) {
            funcgen.selected = (funcgen_control_t)selected; funcgen.last_error = error ? ESP_FAIL : ESP_OK;
            funcgen.enabled = selected % 2 != 0;
            if (argc > 1 && !error && (selected == 1 || selected == 7)) {
                char path[512]; snprintf(path, sizeof(path), "%s/%dx%d-%u.draw", argv[1], gfx.width, gfx.height, selected);
                trace = fopen(path, "w"); assert(trace);
            }
            funcgen_render_graphics(&context);
            assert(texts > 0 && !funcgen.redraw && selected_label_drawn && selected_value_drawn);
            if (gfx.width >= 384 && gfx.height >= 240) assert(circles == 9 && scopes == 1);
            else assert(circles == 0);
            if (gfx.height >= 110 && gfx.width >= 128) assert(scopes == 1);
            if (trace) { fclose(trace); trace = NULL; }
        }
    }
    const size_t dimensions[][2] = {{24, 80}, {12, 44}, {8, 32}, {4, 20}, {3, 16}, {2, 16}, {1, 16}};
    for (size_t size = 0; size < sizeof(dimensions) / sizeof(dimensions[0]); size++) {
        funcgen.tui = (solar_os_tui_t){dimensions[size][0], dimensions[size][1]};
        for (unsigned step = 0; step < 24; step++) {
            funcgen.selected = (funcgen_control_t)(step < 16 ? step % 8 : 7 - step % 8);
            funcgen_render_tui();
            const size_t top = funcgen.tui.rows >= 3 ? 1 : 0;
            const size_t row = top + (size_t)funcgen.selected - funcgen.controls.top;
            assert(row < funcgen.tui.rows - (funcgen.tui.rows >= 2));
            assert(screen[row][0] != 0 && !funcgen.redraw);
            if (funcgen.tui.cols >= 16) {
                const char *expected = values[funcgen.selected];
                assert(strstr(screen[row], funcgen.selected == FUNCGEN_CONTROL_OUTPUT ? "runtime" : expected));
            }
        }
    }
    puts("Funcgen controls fit nine graphical sizes and seven terminal sizes");
    return 0;
}
