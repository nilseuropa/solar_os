#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/apps/solar_os_io.c"

#define TEST_ROWS 20U
#define TEST_COLS 160U
static uint32_t screen[TEST_ROWS][TEST_COLS];
static size_t screen_cols, screen_rows = TEST_ROWS, cursor_col, cursor_row;
static bool cursor_visible;
static io_state_t state;
static solar_os_bus_info_t bus;
static size_t test_pin_count = 1U;
#if SOLAR_OS_PACKAGE_CAN_TWAI
static bool can_supported = true, can_attached;
static esp_err_t can_attach_result = ESP_OK, can_detach_result = ESP_OK;
static size_t can_attach_calls;
static solar_os_expansion_device_t can_device;
static char startup_command[IO_STARTUP_COMMAND_MAX];
#endif

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

size_t solar_os_tui_cols(const solar_os_tui_t *tui)
{
    (void)tui;
    return screen_cols;
}

size_t solar_os_tui_rows(const solar_os_tui_t *tui)
{
    (void)tui;
    return screen_rows;
}

void solar_os_tui_clear(solar_os_tui_t *tui)
{
    (void)tui;
    for (size_t row = 0; row < TEST_ROWS; row++)
        for (size_t col = 0; col < TEST_COLS; col++) screen[row][col] = ' ';
}

void solar_os_tui_refresh(solar_os_tui_t *tui) { (void)tui; }

esp_err_t solar_os_tui_fill(solar_os_tui_t *tui, size_t row, size_t col,
                            size_t height, size_t width, uint32_t ch, uint8_t attr)
{
    (void)tui;
    (void)attr;
    assert(row + height <= screen_rows && col + width <= screen_cols);
    for (size_t y = row; y < row + height; y++)
        for (size_t x = col; x < col + width; x++) screen[y][x] = ch;
    return ESP_OK;
}

esp_err_t solar_os_tui_putch(solar_os_tui_t *tui, size_t row, size_t col,
                             uint32_t ch, uint8_t attr)
{
    (void)tui;
    (void)attr;
    assert(row < screen_rows && col < screen_cols);
    screen[row][col] = ch;
    return ESP_OK;
}

esp_err_t solar_os_tui_set_cursor_visible(solar_os_tui_t *tui, bool visible)
{
    (void)tui;
    cursor_visible = visible;
    return ESP_OK;
}

esp_err_t solar_os_tui_move(solar_os_tui_t *tui, size_t row, size_t col)
{
    (void)tui;
    assert(row < screen_rows && col < screen_cols);
    cursor_row = row;
    cursor_col = col;
    return ESP_OK;
}

size_t solar_os_bus_count(void) { return 1U; }
bool solar_os_bus_get(size_t index, solar_os_bus_info_t *info)
{
    if (index != 0U) return false;
    *info = bus;
    return true;
}
const char *solar_os_bus_protocol_name(solar_os_bus_protocol_t protocol)
{
    return protocol == SOLAR_OS_BUS_PROTOCOL_UART ? "uart" : "spi";
}

bool solar_os_bus_runtime_protocol_available(solar_os_bus_protocol_t protocol)
{ (void)protocol; return true; }
size_t solar_os_bus_runtime_endpoint_count(solar_os_bus_protocol_t protocol)
{ (void)protocol; return 1U; }
bool solar_os_bus_runtime_endpoint_get(solar_os_bus_protocol_t protocol, size_t index, int *endpoint)
{ (void)protocol; if (index != 0U) return false; *endpoint = 1; return true; }
esp_err_t solar_os_bus_register(const solar_os_bus_definition_t *definition)
{ (void)definition; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_bus_unregister(const char *name)
{ (void)name; return ESP_ERR_NOT_SUPPORTED; }

size_t solar_os_pin_count(void) { return test_pin_count; }
bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *info)
{
    if (index >= test_pin_count) return false;
    *info = (solar_os_pin_info_t){.pin = 43 + (int)index, .expansion = true,
        .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "long pin role"};
    return true;
}
bool solar_os_pin_is_routable(int pin)
{ return pin >= 43 && pin < 43 + (int)test_pin_count; }
bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info)
{
    return pin == 43 && solar_os_pin_get_info(0U, info);
}
const char *solar_os_pin_policy_name(solar_os_pin_policy_t policy)
{
    (void)policy;
    return "releasable";
}
bool solar_os_gpio_get_pin_info_by_pin(int pin, solar_os_gpio_pin_info_t *info)
{
    (void)pin;
    (void)info;
    return false;
}
const char *solar_os_gpio_mode_name(solar_os_gpio_mode_t mode)
{
    (void)mode;
    return "input";
}
bool solar_os_resource_find_claim(solar_os_resource_kind_t kind, int primary,
                                  int secondary, solar_os_resource_claim_t *claim)
{
    (void)kind;
    (void)primary;
    (void)secondary;
    (void)claim;
    return false;
}
size_t solar_os_resource_claim_count(void) { return 1U; }
bool solar_os_resource_get_claim(size_t index, solar_os_resource_claim_t *claim)
{
    if (index != 0U) return false;
    *claim = (solar_os_resource_claim_t){.kind = SOLAR_OS_RESOURCE_GPIO_PIN,
        .primary = 43, .secondary = -1};
    strlcpy(claim->owner, "bus:receiver_long_name", sizeof(claim->owner));
    strlcpy(claim->label, "external receiver", sizeof(claim->label));
    return true;
}
const char *solar_os_resource_kind_name(solar_os_resource_kind_t kind)
{
    (void)kind;
    return "gpio";
}

#if SOLAR_OS_PACKAGE_CAN_TWAI
bool solar_os_expansion_driver_supported(const char *name)
{ return can_supported && strcmp(name, "twai-can") == 0; }
size_t solar_os_expansion_device_count(void) { return can_attached ? 2U : 1U; }
bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device)
{
    if (index == 0U) {
        *device = (solar_os_expansion_device_t){.driver = "other", .name = "sensor0"};
        return true;
    }
    if (index != 1U || !can_attached) return false;
    *device = can_device;
    return true;
}
esp_err_t solar_os_expansion_attach(const char *driver, const char *name,
                                     const solar_os_expansion_binding_t *bindings, size_t count)
{
    assert(strcmp(driver, "twai-can") == 0 && count <= 6U);
    can_attach_calls++;
    if (can_attach_result != ESP_OK) return can_attach_result;
    can_device = (solar_os_expansion_device_t){.active = true, .ready = true,
        .detachable = true, .origin = SOLAR_OS_EXPANSION_ORIGIN_RUNTIME, .binding_count = count};
    strlcpy(can_device.name, name, sizeof(can_device.name));
    strlcpy(can_device.driver, driver, sizeof(can_device.driver));
    memcpy(can_device.bindings, bindings, count * sizeof(*bindings));
    can_attached = true;
    return ESP_OK;
}
esp_err_t solar_os_expansion_detach(const char *name)
{
    assert(can_attached && strcmp(name, can_device.name) == 0);
    if (can_detach_result == ESP_OK) can_attached = false;
    return can_detach_result;
}
size_t solar_os_expansion_driver_count(void) { return 1U; }
bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver)
{
    static const solar_os_expansion_binding_spec_t specs[] = {
        {.key = "tx", .role = "tx", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
        {.key = "rx", .role = "rx", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO},
        {.key = "bitrate", .role = "bitrate", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER},
        {.key = "listen", .role = "listen", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER},
        {.key = "active", .role = "active", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER},
        {.key = "select", .role = "select", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE},
    };
    if (index != 0U) return false;
    *driver = (solar_os_expansion_driver_t){.name = "twai-can", .binding_specs = specs,
        .binding_spec_count = sizeof(specs) / sizeof(specs[0])};
    return true;
}
esp_err_t solar_os_shell_startup_append_command(const char *command, bool *added)
{
    *added = strcmp(startup_command, command) != 0;
    strlcpy(startup_command, command, sizeof(startup_command));
    return ESP_OK;
}
#endif
bool solar_os_connector_layout_get_info(solar_os_connector_layout_info_t *info)
{
    *info = (solar_os_connector_layout_info_t){.title = "Connectors", .view = "Top",
        .rows = 2U, .columns = 2U};
    return true;
}
size_t solar_os_connector_pin_count(void) { return 4U; }
bool solar_os_connector_pin_find(size_t row, size_t col,
                                 solar_os_connector_pin_info_t *info, size_t *index)
{
    if (row >= 2U || col >= 2U) return false;
    *info = (solar_os_connector_pin_info_t){.connector = "GROVE", .position = row * 2U + col + 1U,
        .row = row, .column = col, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO,
        .pin = 43, .label = "GPIO43"};
    if (index != NULL) *index = row * 2U + col;
    return true;
}
bool solar_os_connector_pin_get_info(size_t index, solar_os_connector_pin_info_t *info)
{
    return solar_os_connector_pin_find(index / 2U, index % 2U, info, NULL);
}
const char *solar_os_connector_pin_kind_name(solar_os_connector_pin_kind_t kind)
{
    (void)kind;
    return "GPIO";
}

static const char *row_text(size_t row)
{
    static char text[TEST_COLS + 1U];
    for (size_t col = 0U; col < screen_cols; col++) {
        text[col] = screen[row][col] < 128U ? (char)screen[row][col] : '?';
    }
    text[screen_cols] = '\0';
    return text;
}

static void reset(size_t cols)
{
    memset(&state, 0, sizeof(state));
    io_state = &state;
    screen_cols = cols;
    screen_rows = TEST_ROWS;
    solar_os_tui_clear(&state.tui);
}

static void test_tables(void)
{
    /* Exercise the real renderers and shared UTF-8 cell writer at every width.
     * Strict screen mocks reject any write beyond the available rectangle. */
    for (size_t cols = 1U; cols <= TEST_COLS; cols++) {
        reset(cols);
        io_render_title();
        io_render_pins();
        io_render_buses();
        io_render_claims();
        io.selected[IO_VIEW_LAYOUT] = 3U;
        io_render_layout();
        io.form.protocol = SOLAR_OS_BUS_PROTOCOL_SPI;
        io.form.max_transfer = 65536U;
        strlcpy(io.form.name, "long_runtime_bus_name", sizeof(io.form.name));
        io_render_form();
        io.form.editing_name = true;
        io_render_form();
        io_render_actions();
        io_render_protocols();
        io_render_confirm();
        io.mode = IO_MODE_I2C_SPEED;
        io_render();
    }
    reset(26U);
    io.view = IO_VIEW_CLAIMS;
    io_render_title();
    assert(strstr(row_text(0), "[Claims]") != NULL);
    io_render_buses();
    const char *heading = row_text(1U);
    const char *state_heading = strstr(heading, "STATE");
    assert(state_heading != NULL);
    const size_t state_col = (size_t)(state_heading - heading);
    assert(strstr(heading, "ROUTING") == NULL);
    assert(strncmp(row_text(2U) + state_col, "detached", 8U) == 0);
    io_render_claims();
    heading = row_text(1U);
    const char *owner_heading = strstr(heading, "OWNER");
    assert(owner_heading != NULL);
    const size_t owner_col = (size_t)(owner_heading - heading);
    assert(strncmp(row_text(2U) + owner_col, "bus:", 4U) == 0);
    screen_cols = 20U;
    io_render_buses();
    assert(strstr(row_text(2U), "detached") != NULL);
    io_render_claims();
    assert(strstr(row_text(2U), "bus:") != NULL);
    screen_cols = 26U;
    io.selected[IO_VIEW_LAYOUT] = 3U;
    io_render_layout();
    assert(strstr(row_text(3U), "GROVE.4 GPIO43") != NULL);
    reset(80U);
    io_render_buses();
    assert(strstr(row_text(1U), "ROUTING") != NULL);
    assert(strstr(row_text(1U), "LEASES") != NULL);
    assert(strstr(row_text(2U), "uart1 TX=43 RX=44") != NULL);
}

static void test_form(void)
{
    reset(26U);
    io.form.protocol = SOLAR_OS_BUS_PROTOCOL_SPI;
    io.form.max_transfer = 65536U;
    strlcpy(io.form.name, "runtime_bus_123", sizeof(io.form.name));
    io_render_form();
    assert(strstr(row_text(10U), "65536 bytes") != NULL);
    assert(strchr(row_text(10U), ']') != NULL);
    io.form.editing_name = true;
    io_render_form();
    assert(strstr(row_text(1U), "time_bus_123") != NULL);
    assert(cursor_visible && cursor_row == 1U && cursor_col == 24U);
    /* Re-render the same active form after a terminal resize. */
    screen_cols = 20U;
    io_render_form();
    assert(strstr(row_text(1U), "_bus_123") != NULL);
    assert(cursor_col == 18U);
    io.form.name[0] = '\0';
    io_render_form();
    assert(cursor_col == 10U && strchr(row_text(1U), '[') != NULL);
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_actions();
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_protocols();
    assert(strstr(row_text(TEST_ROWS - 1U), "Esc") != NULL);
    io_render_browse();
    assert(strstr(row_text(TEST_ROWS - 1U), "Q:exit") != NULL);
}

static void test_protocols(void)
{
    reset(40U);
    io_protocols_build();
#if SOLAR_OS_PACKAGE_CAN_TWAI
    assert(io.protocol_count == 7U && io.protocols[6].can);
    can_supported = false;
    io_protocols_build();
    assert(io.protocol_count == 6U);
    can_supported = true;
#else
    assert(io.protocol_count == 6U);
#endif
}

#if SOLAR_OS_PACKAGE_CAN_TWAI
static void test_can(void)
{
    reset(80U);
    test_pin_count = 3U;
    io.selected_pin = 44;
    io_form_begin((io_protocol_t){.can = true});
    assert(strcmp(io.form.name, "can0") == 0 && io_form_field_count() == 8U);
    assert(io.form.pins[0] == 44 && io.form.pins[1] == 43 && io.form.pins[2] == -1);
    assert(io.form.rate == 500U && io.form.listen_only && io.form.select_active);
    can_attach_result = ESP_ERR_INVALID_STATE;
    assert(io_form_create() == ESP_ERR_INVALID_STATE && !can_attached);
    can_attach_result = ESP_OK;
    assert(io_form_create() == ESP_OK && can_attached);
    assert(can_device.binding_count == 5U && io_can_value(&can_device, "listen", -1) == 1);
    assert(io_bus_count() == 2U);
    io_render_buses();
    assert(strstr(row_text(3U), "can0") && strstr(row_text(3U), "can"));
    size_t calls = can_attach_calls;
    assert(io_form_create() == ESP_ERR_INVALID_STATE && can_attach_calls == calls);
    io_build_can_actions(&can_device);
    assert(io.selected_can && io.actions[0].kind == IO_ACTION_BUS_AUTOSTART);
    assert(io.actions[1].kind == IO_ACTION_BUS_REMOVE);
    bool added;
    assert(io_autostart_bus("can0", &added) == ESP_OK && added);
    assert(strstr(startup_command, "expansion attach twai-can can0"));
    assert(strstr(startup_command, "bitrate=500") && strstr(startup_command, "listen=1"));
    assert(io_autostart_bus("can0", &added) == ESP_OK && !added);
    can_detach_result = ESP_ERR_INVALID_STATE;
    assert(io_remove_bus() == ESP_ERR_INVALID_STATE && io_bus_count() == 2U);
    can_detach_result = ESP_OK;
    assert(io_remove_bus() == ESP_OK && io_bus_count() == 1U);

    io.form.selected = 3U;
    io_form_cycle(1);
    assert(io.form.rate == 800U);
    io.form.selected = 4U;
    io_form_cycle(1);
    assert(!io.form.listen_only);
    io.form.selected = 5U;
    io_form_cycle(1);
    assert(io.form.pins[2] == 45);
    io.form.selected = 6U;
    io_form_cycle(1);
    assert(!io.form.select_active);
    strlcpy(io.form.name, "can test", sizeof(io.form.name));
    assert(io_form_create() == ESP_OK && can_device.binding_count == 6U);
    assert(io_can_value(&can_device, "select", -1) == 45);
    io.selected_can = true;
    assert(io_autostart_bus("can test", &added) == ESP_OK && added);
    assert(strstr(startup_command, "\"can test\"") && strstr(startup_command, "select=gpio45"));
    assert(strstr(startup_command, "listen=0") && strstr(startup_command, "active=0"));
    /* Render the CAN form and controller row at every supported screen width. */
    for (size_t cols = 1U; cols <= TEST_COLS; cols++) {
        screen_cols = cols;
        io_render_form();
        io_render_buses();
    }
    screen_cols = 40U;
    screen_rows = 8U;
    io_protocols_build();
    io.protocol_selected = 6U;
    io_render_protocols();
    assert(strstr(row_text(6U), "can") && strstr(row_text(7U), "Esc"));
    io.form.selected = 7U;
    io_render_form();
    assert(strstr(row_text(5U), "Create") && strstr(row_text(7U), "Esc"));
    screen_rows = TEST_ROWS;
    can_attached = false;
    test_pin_count = 1U;
}
#endif

int main(void)
{
    bus.protocol = SOLAR_OS_BUS_PROTOCOL_UART;
    bus.config.uart.port = 1;
    bus.config.uart.tx_pin = 43;
    bus.config.uart.rx_pin = 44;
    bus.config.uart.baud_rate = 115200U;
    strlcpy(bus.name, "receiver_long_name", sizeof(bus.name));
    test_tables();
    test_form();
    test_protocols();
#if SOLAR_OS_PACKAGE_CAN_TWAI
    test_can();
#endif
    puts("I/O layout tests passed");
    return 0;
}
