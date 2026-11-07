#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "solar_os_terminal_internal.h"

#define TERM_MARGIN_X 4
#define TERM_CURSOR_BLINK_MS 500U
#define SOLAR_OS_DISPLAY_PRESENT_TEXT 0

extern const uint8_t u8g2_font_5x8_tf[];
static uint8_t pixels[240 * 64 / 8];
static uint32_t now_ms;
static unsigned presents;
static bool display_available = true;
static uint32_t display_epoch;
static solar_os_terminal_cursor_shape_t preference_shape;
static bool preference_blink = true;

int64_t esp_timer_get_time(void) { return (int64_t)now_ms * 1000; }
void solar_os_terminal_get_cursor_preference(solar_os_terminal_cursor_shape_t *shape, bool *blink)
{
    if (shape) *shape = preference_shape;
    if (blink) *blink = preference_blink;
}
static bool solar_os_display_terminal_draw_state(u8g2_t *u, uint32_t *epoch)
{ (void)u; *epoch = display_epoch; return display_available; }
static int solar_os_display_request_present_mode(u8g2_t *u, int mode)
{ (void)u; (void)mode; return 0; }
static void solar_os_display_present(u8g2_t *u, int mode)
{ (void)u; (void)mode; presents++; }
static size_t terminal_rows(const solar_os_terminal_t *t) { return t->rows; }
static size_t terminal_cols(const solar_os_terminal_t *t) { return t->cols; }
static void terminal_return_to_live(solar_os_terminal_t *t) { t->scrollback_offset = 0; }
static void terminal_apply_settings(solar_os_terminal_t *t, bool clear)
{ (void)t; (void)clear; }
static uint8_t terminal_text_scale(const solar_os_terminal_t *t) { (void)t; return 1; }
static const uint8_t *terminal_selected_font(const solar_os_terminal_t *t, bool b, bool i)
{ (void)t; (void)b; (void)i; return u8g2_font_5x8_tf; }
static bool terminal_draw_box_cell(u8g2_t *u, int x, int y, int w, int h, uint16_t c)
{ (void)u; (void)x; (void)y; (void)w; (void)h; (void)c; return false; }
#define terminal_draw_block_cell terminal_draw_box_cell
static bool attr_get(const uint32_t *words, size_t col)
{ return (words[col / 32] & (1U << (col % 32))) != 0; }
#define terminal_bold_get attr_get
#define terminal_italic_get attr_get
#define terminal_underline_get attr_get
#define terminal_inverse_get attr_get
static size_t terminal_line_len(const solar_os_terminal_t *t, const uint16_t *line)
{ size_t len = 0; while (len < t->cols && line[len]) len++; return len; }
static const uint16_t *terminal_display_line(const solar_os_terminal_t *t, size_t row,
    const uint32_t **bold, const uint32_t **italic, const uint32_t **underline, const uint32_t **inverse)
{
    *bold = t->bold[row]; *italic = t->italic[row];
    *underline = t->underline[row]; *inverse = t->inverse[row];
    return t->lines[row];
}
static void terminal_draw_status_bar(solar_os_terminal_t *t, u8g2_t *u)
{ (void)t; (void)u; }
static void terminal_draw_footer(solar_os_terminal_t *t, u8g2_t *u, uint8_t scale)
{ (void)t; (void)u; (void)scale; }

#include "terminal_cursor.inc"

struct solar_os_shell_io { char output[256]; };
static unsigned saves;
static esp_err_t save_result;
esp_err_t solar_os_terminal_set_cursor_preference(solar_os_terminal_cursor_shape_t shape, bool blink)
{
    saves++;
    if (save_result != ESP_OK) return save_result;
    preference_shape = shape;
    preference_blink = blink;
    return ESP_OK;
}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(io->output + strlen(io->output), sizeof(io->output) - strlen(io->output), format, args);
    va_end(args);
}
static void solar_os_shell_io_writeln(solar_os_shell_io_t *io, const char *text)
{ solar_os_shell_io_printf(io, "%s\n", text); }
static const char *solar_os_shell_error_text(esp_err_t err) { (void)err; return "save error"; }
static void solar_os_shell_diag_unexpected(solar_os_shell_io_t *io,
                                          const char *command, const char *value, const char *usage)
{ (void)command; (void)value; (void)usage; solar_os_shell_io_writeln(io, "unexpected"); }
static void solar_os_shell_diag_invalid(solar_os_shell_io_t *io,
    const char *command, const char *field, const char *value, const char *expected, const char *usage, bool secret)
{
    (void)command; (void)field; (void)value; (void)expected; (void)usage; (void)secret;
    solar_os_shell_io_writeln(io, "invalid");
}
#include "setterm_cursor.inc"

static void test_command(void)
{
    solar_os_shell_io_t io = {0};
    char *args[] = {"setterm", "cursor", "underscore", "steady", "extra"};
    preference_shape = SOLAR_OS_TERMINAL_CURSOR_BLOCK; preference_blink = true;
    setterm_cursor(&io, 2, args);
    assert(strstr(io.output, "cursor: block blink") && saves == 0);
    setterm_cursor(&io, 4, args);
    assert(preference_shape == SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE && !preference_blink);
    args[2] = "block";
    setterm_cursor(&io, 3, args); /* Shape-only preserves the mode. */
    assert(preference_shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK && !preference_blink);
    args[3] = "blink";
    setterm_cursor(&io, 4, args);
    assert(preference_blink);
    const unsigned before = saves;
    setterm_cursor(&io, 5, args);
    args[3] = "off";
    setterm_cursor(&io, 4, args);
    args[2] = "bar";
    setterm_cursor(&io, 3, args);
    assert(saves == before && preference_shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK && preference_blink);
    args[2] = "underscore";
    io.output[0] = 0;
    save_result = ESP_FAIL;
    setterm_cursor(&io, 3, args);
    assert(strstr(io.output, "save failed") && preference_shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK);
    save_result = ESP_OK;
}

static const u8x8_display_info_t info = {
    .tile_width = 30, .tile_height = 8, .pixel_width = 240, .pixel_height = 64,
};
static uint8_t display(u8x8_t *u, uint8_t msg, uint8_t arg, void *ptr)
{
    (void)arg; (void)ptr;
    if (msg == U8X8_MSG_DISPLAY_SETUP_MEMORY) u8x8_d_helper_display_setup_memory(u, &info);
    return 1;
}
static void init_terminal(solar_os_terminal_t *t, u8g2_t *u)
{
    memset(t, 0, sizeof(*t));
    t->u8g2 = u; t->cols = 38; t->rows = 8;
    t->char_width = 6; t->line_height = 8; t->cell_ascent = 7; t->baseline_offset = 7;
    t->black_is_one = true;
    t->cursor_visible = true;
    t->lines[0][0] = 'A'; t->lines[0][1] = 'B';
    terminal_wake_cursor(t);
}
static void assert_cell(const uint8_t *background, size_t col, size_t row,
                        bool inverted, bool underscore)
{
    for (unsigned x = TERM_MARGIN_X + col * 6; x < TERM_MARGIN_X + (col + 1) * 6; x++) {
        uint8_t expected = background[row * 240 + x];
        if (inverted) expected ^= 0xff;
        if (underscore) expected |= 0x40;
        assert(pixels[row * 240 + x] == expected);
    }
}

int main(void)
{
    u8g2_t u;
    u8g2_SetupDisplay(&u, display, u8x8_cad_empty, u8x8_dummy_cb, u8x8_dummy_cb);
    u8g2_SetupBuffer(&u, pixels, 8, u8g2_ll_hvline_vertical_top_lsb, U8G2_R0);
    u8g2_InitDisplay(&u);
    static solar_os_terminal_t t;
    init_terminal(&t, &u);
    uint8_t background[sizeof(pixels)], on[sizeof(pixels)];
    solar_os_terminal_set_cursor_visible(&t, false);
    solar_os_terminal_draw(&t);
    memcpy(background, pixels, sizeof(pixels));
    solar_os_terminal_set_cursor_visible(&t, true);
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, true, false);
    memcpy(on, pixels, sizeof(pixels));
    unsigned count = presents;
    solar_os_terminal_draw(&t);
    assert(presents == count && !memcmp(on, pixels, sizeof(pixels)));
    assert(!solar_os_terminal_needs_draw(&t));

    now_ms = 499;
    assert(!solar_os_terminal_needs_draw(&t));
    now_ms = 500;
    assert(solar_os_terminal_needs_draw(&t));
    solar_os_terminal_draw(&t);
    assert(!memcmp(background, pixels, sizeof(pixels)));
    assert(solar_os_terminal_cursor_visible(&t)); /* App-visible is independent of blink phase. */
    now_ms = 1000;
    assert(solar_os_terminal_needs_draw(&t));
    solar_os_terminal_draw(&t);
    assert(!memcmp(on, pixels, sizeof(pixels)));

    /* Other-row changes cannot invert the cached cursor twice. */
    t.lines[1][0] = 'X'; t.dirty = true;
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, true, false);
    now_ms = 1500;
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, false, false);
    /* Moving while off wakes the cursor and restores the previous cell. */
    solar_os_terminal_set_cursor(&t, 0, 1);
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, false, false);
    assert_cell(background, 1, 0, true, false);

    preference_shape = SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE;
    preference_blink = false;
    assert(solar_os_terminal_needs_draw(&t));
    solar_os_terminal_draw(&t);
    assert_cell(background, 1, 0, false, true);
    now_ms += 501;
    assert(!solar_os_terminal_needs_draw(&t));
    preference_blink = true;
    solar_os_terminal_draw(&t);
    now_ms += 500;
    solar_os_terminal_draw(&t);
    assert_cell(background, 1, 0, false, false);

    /* App-hidden and scrollback cursors stay hidden throughout blinking. */
    solar_os_terminal_set_cursor_visible(&t, false);
    solar_os_terminal_draw(&t);
    now_ms += 1000;
    assert(!solar_os_terminal_needs_draw(&t));
    solar_os_terminal_set_cursor_visible(&t, true);
    t.scrollback_offset = 1;
    solar_os_terminal_draw(&t);
    assert(!t.rendered_cursor_on);
    now_ms += 500;
    assert(!solar_os_terminal_needs_draw(&t));

    /* Steady block and reverse-video cells remain readable. */
    preference_shape = SOLAR_OS_TERMINAL_CURSOR_BLOCK;
    preference_blink = false;
    t.scrollback_offset = 0; t.inverse[0][0] = 2;
    solar_os_terminal_set_cursor_visible(&t, false);
    solar_os_terminal_draw(&t);
    memcpy(background, pixels, sizeof(pixels));
    solar_os_terminal_set_cursor_visible(&t, true);
    solar_os_terminal_draw(&t);
    assert_cell(background, 1, 0, true, false);
    now_ms += 1500;
    assert(!solar_os_terminal_needs_draw(&t));

    /* Bottom/right edge and a ruler crossing the cursor cell. */
    solar_os_terminal_set_cursor(&t, 7, t.cols);
    solar_os_terminal_draw(&t);
    for (unsigned x = 226; x < 232; x++) assert(pixels[7 * 240 + x] == 0xff);
    solar_os_terminal_set_cursor(&t, 0, 0);
    t.vrules[0] = (solar_os_terminal_vrule_t){.row=0, .col=0, .height=2, .width=1};
    t.vrule_count = 1;
    solar_os_terminal_draw(&t);
    memcpy(on, pixels, sizeof(pixels));
    t.lines[1][0] = 'Y'; t.dirty = true;
    solar_os_terminal_draw(&t);
    assert(!memcmp(on, pixels, 240));

    /* Palette/pixel polarity changes retain the same inversion semantics. */
    t.palette_inverted = true;
    solar_os_terminal_set_cursor_visible(&t, false);
    solar_os_terminal_draw(&t);
    memcpy(background, pixels, sizeof(pixels));
    solar_os_terminal_set_cursor_visible(&t, true);
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, true, false);

    display_available = false;
    now_ms += 500;
    assert(!solar_os_terminal_needs_draw(&t));
    display_available = true;
    display_epoch++;
    assert(solar_os_terminal_needs_draw(&t));
    solar_os_terminal_draw(&t);
    assert_cell(background, 0, 0, true, false);

    /* Fresh sessions use the shared preference, and time wrap keeps blinking. */
    preference_blink = true;
    init_terminal(&t, &u);
    solar_os_terminal_draw(&t);
    assert(t.rendered_cursor_shape == SOLAR_OS_TERMINAL_CURSOR_BLOCK && t.rendered_cursor_blink);
    now_ms = UINT32_MAX - 100;
    terminal_wake_cursor(&t);
    solar_os_terminal_draw(&t);
    now_ms += 500;
    solar_os_terminal_draw(&t);
    assert(!t.rendered_cursor_on);
    puts("terminal cursor pixels, caching, blink, visibility, edges and rollover passed");
    test_command();
    puts("setterm cursor query, updates, validation and save-error reporting passed");
    return 0;
}
