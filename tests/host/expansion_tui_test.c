#include <assert.h>
#include "../../src/shell/solar_os_shell_expansion_tui.c"

static expansion_tui_state_t state;
static size_t rows = 6, cols = 32, cursor_row, cursor_col;
static bool cursor_visible;
static char screen[24][81];

size_t strlcpy(char *dst, const char *src, size_t capacity)
{
    size_t len = strlen(src);
    if (capacity) { size_t n = len < capacity - 1 ? len : capacity - 1;
        memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
size_t solar_os_tui_rows(const solar_os_tui_t *tui) { (void)tui; return rows; }
size_t solar_os_tui_cols(const solar_os_tui_t *tui) { (void)tui; return cols; }
esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{ (void)tui; cursor_visible = visible; return ESP_OK; }
esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{ (void)tui; assert(row < rows && col < cols); cursor_row = row; cursor_col = col; return ESP_OK; }
esp_err_t solar_os_tui_putch(solar_os_tui_t *tui, size_t row, size_t col, uint32_t ch, uint8_t attr)
{
    (void)tui; (void)attr; assert(row < rows && col < cols);
    screen[row][col] = ch < 128 ? (char)ch : '?';
    cursor_row = row; cursor_col = col + 1;
    return ESP_OK;
}
esp_err_t solar_os_tui_fill(solar_os_tui_t *tui, size_t row, size_t col,
    size_t height, size_t width, uint32_t ch, uint8_t attr)
{
    for (size_t y = row; y < row + height; y++)
        for (size_t x = col; x < col + width; x++) solar_os_tui_putch(tui, y, x, ch, attr);
    return ESP_OK;
}
void solar_os_tui_clear(solar_os_tui_t *tui)
{
    (void)tui;
    for (size_t y = 0; y < rows; y++) { memset(screen[y], ' ', cols); screen[y][cols] = 0; }
    cursor_row = cursor_col = 0;
}
void solar_os_tui_refresh(solar_os_tui_t *tui) { (void)tui; }
size_t solar_os_pin_count(void) { return 0; }
bool solar_os_pin_get_info(size_t i, solar_os_pin_info_t *out) { (void)i; (void)out; return false; }
bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *out) { (void)pin; (void)out; return false; }
size_t solar_os_connector_pin_count(void) { return 0; }
bool solar_os_connector_pin_get_info(size_t i, solar_os_connector_pin_info_t *out)
{ (void)i; (void)out; return false; }
bool solar_os_resource_find_claim(solar_os_resource_kind_t kind, int primary, int secondary,
    solar_os_resource_claim_t *out)
{ (void)kind; (void)primary; (void)secondary; (void)out; return false; }
bool solar_os_expansion_binding_pin_supported(solar_os_expansion_binding_kind_t kind,
    const char *target, int pin) { (void)kind; (void)target; (void)pin; return false; }
size_t solar_os_bus_count_protocol(solar_os_bus_protocol_t protocol) { (void)protocol; return 0; }
bool solar_os_bus_get_protocol(solar_os_bus_protocol_t protocol, size_t i, solar_os_bus_info_t *out)
{ (void)protocol; (void)i; (void)out; return false; }
size_t solar_os_stream_count(void) { return 0; }
bool solar_os_stream_get(size_t i, solar_os_stream_info_t *out) { (void)i; (void)out; return false; }
bool solar_os_expansion_stream_binding_valid(solar_os_expansion_binding_kind_t kind,
    const solar_os_stream_info_t *stream) { (void)kind; (void)stream; return false; }

static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "panel", .value_hint = "0..7", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
     .role = "panel", .has_value_range = true, .max_value = 7},
    {.key = "rotation", .value_hint = "0..3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
     .role = "rotation", .has_value_range = true, .max_value = 3},
};

static void setup(const char *driver)
{
    memset(&state, 0, sizeof(state));
    expansion_tui_state = &state;
    state.view = EXPANSION_TUI_VIEW_ATTACH;
    state.driver = (solar_os_expansion_driver_t){.name = driver, .binding_specs = specs,
        .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
        .binding_spec_count = sizeof(specs) / sizeof(specs[0])};
    strlcpy(state.device_name, "oled0", sizeof(state.device_name));
}

static void test_editor(void)
{
    for (size_t size = 0; size < 3; size++) {
        rows = size == 0 ? 4 : size == 1 ? 6 : 24;
        cols = size == 2 ? 80 : 32;
        setup("ssd1306");
        expansion_tui_begin_edit(0);
        expansion_tui_render_attach();
        assert(cursor_visible && cursor_row == 2 && cursor_col == 13);
        expansion_tui_edit_key('x'); expansion_tui_render_attach();
        assert(cursor_visible && cursor_row == 2 && cursor_col == 14);
        expansion_tui_edit_key('\r'); expansion_tui_render_attach();
        assert(!state.editing && !cursor_visible && state.view == EXPANSION_TUI_VIEW_ATTACH);
        assert(strcmp(state.device_name, "oled0x") == 0);
        expansion_tui_begin_edit(0);
        expansion_tui_edit_key(SOLAR_OS_KEY_ENTER); expansion_tui_render_attach();
        assert(!state.editing && !cursor_visible);
        /* Long values scroll inside the common editor; later rows/footer must
         * not move its cursor. Scrolling the form must keep the editor visible. */
        memset(state.values[1], '9', sizeof(state.values[1]) - 1);
        expansion_tui_begin_edit(2); expansion_tui_render_attach();
        assert(cursor_visible && cursor_col < cols);
        assert(cursor_row == 2 + state.form.cursor - state.form.top);
        expansion_tui_edit_key(SOLAR_OS_KEY_ESCAPE); expansion_tui_render_attach();
        assert(!state.editing && !cursor_visible);
        state.form.cursor = expansion_tui_form_input_count();
        expansion_tui_render_attach(); /* attach button is not an input field */
        assert(!cursor_visible);
    }
}

static void test_panels(void)
{
    setup("ssd1306"); state.choice_field = 1;
    char label[48];
    expansion_tui_form_label(1, label, sizeof(label));
    assert(strcmp(label, "- panel: ") == 0);
    expansion_tui_begin_edit(1);
    expansion_tui_form_label(1, label, sizeof(label));
    assert(strcmp(label, "- panel ID <0..7>: ") == 0);
    expansion_tui_edit_key(SOLAR_OS_KEY_ENTER);
    assert(expansion_tui_field_has_choices(1) && expansion_tui_field_has_choices(2));
    assert(expansion_tui_choice_count() == 9); /* optional default plus eight panels */
    expansion_tui_choice_t choice;
    assert(expansion_tui_get_choice(0, &choice) && choice.value[0] == 0);
    assert(strstr(choice.label, "Default:") && strstr(choice.label, "128x64"));
    assert(expansion_tui_get_choice(2, &choice));
    assert(strcmp(choice.value, "1") == 0 && strstr(choice.label, "128x32") && strstr(choice.label, "Univision"));
    strlcpy(state.values[0], choice.value, sizeof(state.values[0]));
    char line[96]; expansion_tui_attach_row(1, line, sizeof(line), NULL);
    assert(strstr(line, "128x32") && strstr(line, "Univision"));
    assert(state.values[1][0] == 0); /* panel selection leaves rotation optional */
    state.driver.name = "sh1106";
    assert(expansion_tui_choice_count() == 5);
    assert(expansion_tui_get_choice(4, &choice) && strcmp(choice.value, "3") == 0);
    assert(strstr(choice.label, "72x40"));
    state.driver.name = "st7305";
    assert(expansion_tui_choice_count() == 3);
    assert(expansion_tui_get_choice(2, &choice) && strstr(choice.label, "168x384"));
    state.driver.name = "ssd1683";
    assert(expansion_tui_get_choice(0, &choice) && strstr(choice.label, "Waveshare"));
    assert(expansion_tui_get_choice(5, &choice) && strcmp(choice.value, "4") == 0);
    assert(strstr(choice.label, "792x272"));
    assert(!expansion_tui_get_choice(6, &choice));
    state.driver.name = "unknown";
    assert(!expansion_tui_field_has_choices(1));
}

static void test_rotation(void)
{
    setup("ssd1306"); state.choice_field = 2;
    assert(expansion_tui_field_has_choices(2) && expansion_tui_choice_count() == 5);
    expansion_tui_choice_t choice;
    assert(expansion_tui_get_choice(0, &choice) && choice.value[0] == 0);
    assert(strcmp(choice.label, "<default>") == 0);
    for (unsigned rotation = 0; rotation < 4; rotation++) {
        assert(expansion_tui_get_choice(rotation + 1, &choice));
        assert(strtoul(choice.value, NULL, 10) == rotation);
        char expected[32]; snprintf(expected, sizeof(expected), "%u degrees", rotation * 90);
        assert(strcmp(choice.label, expected) == 0);
    }
    assert(!expansion_tui_get_choice(5, &choice));
    strlcpy(state.values[1], "3", sizeof(state.values[1]));
    char line[96]; expansion_tui_attach_row(2, line, sizeof(line), NULL);
    assert(strcmp(line, "- rotation: 270 degrees") == 0);
    /* Drivers such as ST7305 allow only quarter-turn orientations. */
    static const int allowed[] = {1, 3};
    solar_os_expansion_binding_spec_t restricted[2];
    memcpy(restricted, specs, sizeof(restricted));
    restricted[1].allowed_values = allowed;
    restricted[1].allowed_value_count = 2;
    state.driver.binding_specs = restricted;
    assert(expansion_tui_choice_count() == 3);
    assert(expansion_tui_get_choice(1, &choice) && strcmp(choice.value, "1") == 0);
    assert(strcmp(choice.label, "90 degrees") == 0);
    assert(expansion_tui_get_choice(2, &choice) && strcmp(choice.value, "3") == 0);
    assert(strcmp(choice.label, "270 degrees") == 0);
    restricted[1].allowed_value_count = 0;
    restricted[1].min_value = 1; restricted[1].max_value = 2;
    assert(expansion_tui_choice_count() == 3);
    assert(expansion_tui_get_choice(2, &choice) && strcmp(choice.value, "2") == 0);
    restricted[1].required = true;
    assert(expansion_tui_choice_count() == 2);
}

int main(void)
{
    test_editor(); test_panels(); test_rotation();
    puts("expansion_tui_test: cursor, Enter acceptance and runtime panel choices PASS");
    return 0;
}
