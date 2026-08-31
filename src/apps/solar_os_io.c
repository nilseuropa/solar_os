#include "solar_os_io.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "solar_os_board_caps.h"
#include "solar_os_buses.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_CONTROLS
#include "solar_os_controls.h"
#endif
#include "solar_os_gpio.h"
#include "solar_os_keys.h"
#include "solar_os_pins.h"
#include "solar_os_resources.h"
#include "solar_os_storage.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#if SOLAR_OS_PACKAGE_SERVICE_ADC
#include "solar_os_adc.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
#include "solar_os_pwm.h"
#endif

#define IO_MESSAGE_MAX 96
#define IO_ACTION_MAX 10
#define IO_PIN_MAX 64
#define IO_FORM_CS_MAX 4
#define IO_STARTUP_DIR ".shell"
#define IO_STARTUP_FILE "startup"
#define IO_STARTUP_COMMAND_MAX 256

typedef enum {
    IO_VIEW_LAYOUT,
    IO_VIEW_PINS,
    IO_VIEW_BUSES,
    IO_VIEW_CLAIMS,
    IO_VIEW_COUNT,
} io_view_t;

typedef enum {
    IO_MODE_BROWSE,
    IO_MODE_ACTIONS,
    IO_MODE_PROTOCOL,
    IO_MODE_FORM,
    IO_MODE_I2C_SPEED,
    IO_MODE_CONFIRM,
} io_mode_t;

typedef enum {
    IO_ACTION_NONE,
    IO_ACTION_GPIO_INPUT,
    IO_ACTION_GPIO_OUTPUT_LOW,
    IO_ACTION_GPIO_OUTPUT_HIGH,
    IO_ACTION_GPIO_RELEASE,
    IO_ACTION_PWM_START,
    IO_ACTION_PWM_STOP,
    IO_ACTION_ADC_READ,
    IO_ACTION_BUS_ATTACH,
    IO_ACTION_BUS_DETACH,
    IO_ACTION_BUS_I2C_SPEED,
    IO_ACTION_BUS_AUTOSTART,
    IO_ACTION_BUS_REMOVE,
    IO_ACTION_NEW_BUS,
} io_action_kind_t;

typedef struct {
    io_action_kind_t kind;
    char label[32];
} io_action_t;

typedef struct {
    solar_os_bus_protocol_t protocol;
    char name[SOLAR_OS_BUS_NAME_MAX];
    char original_name[SOLAR_OS_BUS_NAME_MAX];
    int endpoint;
    int pins[8];
    uint32_t rate;
    uint32_t max_transfer;
    size_t selected;
    bool editing_name;
} io_form_t;

typedef struct {
    solar_os_tui_t tui;
    solar_os_context_t *ctx;
    io_view_t view;
    io_mode_t mode;
    size_t selected[IO_VIEW_COUNT];
    size_t protocol_selected;
    solar_os_bus_protocol_t protocols[6];
    size_t protocol_count;
    io_action_t actions[IO_ACTION_MAX];
    size_t action_count;
    size_t action_selected;
    io_action_kind_t confirm_action;
    int selected_pin;
    char selected_bus[SOLAR_OS_BUS_NAME_MAX];
    char message[IO_MESSAGE_MAX];
    io_form_t form;
    size_t i2c_rate_selected;
} io_state_t;

static void *io_state;
#define io (*(io_state_t *)io_state)

static const uint32_t i2c_rates[] = {100000U, 400000U, 1000000U};
static const uint32_t uart_rates[] = {9600U, 19200U, 38400U, 57600U, 115200U, 230400U, 460800U, 921600U};
static const uint32_t midi_rates[] = {31250U, 38400U, 57600U, 115200U};
static const uint32_t spi_sizes[] = {256U, 1024U, 4096U, 8192U, 16384U, 65536U};

static const char *io_view_name(io_view_t view)
{
    switch (view) {
    case IO_VIEW_LAYOUT:
        return "Layout";
    case IO_VIEW_PINS:
        return "Pins";
    case IO_VIEW_BUSES:
        return "Buses";
    case IO_VIEW_CLAIMS:
        return "Claims";
    default:
        return "I/O";
    }
}

static void io_set_message(const char *message)
{
    strlcpy(io.message, message != NULL ? message : "", sizeof(io.message));
}

static void io_set_error(const char *operation, esp_err_t err)
{
    snprintf(io.message,
             sizeof(io.message),
             "%s: %s",
             operation != NULL ? operation : "operation",
             esp_err_to_name(err));
}

static void io_write_row(size_t row, const char *text, uint8_t attr)
{
    const size_t cols = solar_os_tui_cols(&io.tui);
    if (cols == 0) {
        return;
    }
    solar_os_tui_write_cell(&io.tui, row, 0, cols, text, attr);
}

static size_t io_expansion_pin_count(void)
{
    size_t count = 0;
    for (size_t i = 0; i < solar_os_pin_count(); i++) {
        solar_os_pin_info_t info;
        if (solar_os_pin_get_info(i, &info) && info.expansion) {
            count++;
        }
    }
    return count;
}

static bool io_expansion_pin_get(size_t index, solar_os_pin_info_t *info)
{
    size_t current = 0;
    for (size_t i = 0; i < solar_os_pin_count(); i++) {
        solar_os_pin_info_t candidate;
        if (!solar_os_pin_get_info(i, &candidate) || !candidate.expansion) {
            continue;
        }
        if (current++ == index) {
            if (info != NULL) {
                *info = candidate;
            }
            return true;
        }
    }
    return false;
}

static bool io_bus_uses_pin(const solar_os_bus_info_t *bus,
                            int pin,
                            char *assignment,
                            size_t assignment_size)
{
    if (bus == NULL) {
        return false;
    }
    const char *signal = NULL;
    switch (bus->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        signal = pin == bus->config.i2c.sda_pin ? "SDA" :
            pin == bus->config.i2c.scl_pin ? "SCL" : NULL;
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        signal = pin == bus->config.spi.sclk_pin ? "SCLK" :
            pin == bus->config.spi.mosi_pin ? "MOSI" :
            pin == bus->config.spi.miso_pin ? "MISO" : NULL;
        for (size_t i = 0; signal == NULL && i < bus->config.spi.cs_count; i++) {
            if (pin == bus->config.spi.cs[i].pin) {
                signal = bus->config.spi.cs[i].name;
            }
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        signal = pin == bus->config.uart.tx_pin ? "TX" :
            pin == bus->config.uart.rx_pin ? "RX" : NULL;
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        signal = pin == bus->config.onewire.pin ? "1W" : NULL;
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        signal = pin == bus->config.ps2.clock_pin ? "CLOCK" :
            pin == bus->config.ps2.data_pin ? "DATA" : NULL;
        break;
    default:
        break;
    }
    if (signal == NULL) {
        return false;
    }
    if (assignment != NULL && assignment_size > 0) {
        snprintf(assignment,
                 assignment_size,
                 "%s:%s%s",
                 bus->name,
                 signal,
                 bus->attached ? "" : " (off)");
    }
    return true;
}

static bool io_find_bus_for_pin(int pin,
                                solar_os_bus_info_t *bus,
                                char *assignment,
                                size_t assignment_size)
{
    solar_os_resource_claim_t claim;
    if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim) &&
        strncmp(claim.owner, "bus:", 4) == 0) {
        for (size_t i = 0; i < solar_os_bus_count(); i++) {
            solar_os_bus_info_t candidate;
            if (solar_os_bus_get(i, &candidate) &&
                strcmp(candidate.name, claim.owner + 4) == 0 &&
                io_bus_uses_pin(&candidate, pin, assignment, assignment_size)) {
                if (bus != NULL) {
                    *bus = candidate;
                }
                return true;
            }
        }
    }
    for (size_t i = 0; i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t candidate;
        if (solar_os_bus_get(i, &candidate) &&
            io_bus_uses_pin(&candidate, pin, assignment, assignment_size)) {
            if (bus != NULL) {
                *bus = candidate;
            }
            return true;
        }
    }
    return false;
}

#if SOLAR_OS_PACKAGE_SERVICE_PWM
static bool io_pwm_info_for_pin(int pin, solar_os_pwm_pin_info_t *info)
{
    for (size_t i = 0; i < solar_os_pwm_pin_count(); i++) {
        solar_os_pwm_pin_info_t candidate;
        if (solar_os_pwm_get_pin_info(i, &candidate) && candidate.pin == pin) {
            if (info != NULL) {
                *info = candidate;
            }
            return true;
        }
    }
    return false;
}
#endif

#if SOLAR_OS_PACKAGE_SERVICE_ADC
static bool io_pin_adc_capable(int pin)
{
    for (size_t i = 0; i < solar_os_adc_pin_count(); i++) {
        solar_os_adc_pin_info_t info;
        if (solar_os_adc_get_pin_info(i, &info) && info.pin == pin) {
            return info.adc_capable;
        }
    }
    return false;
}
#endif

/* Controls retain source associations while ADC resource claims are per-read. */
static bool io_control_uses_pin(int pin,
                                char *assignment,
                                size_t assignment_size,
                                char *owner,
                                size_t owner_size)
{
#if !SOLAR_OS_PACKAGE_SERVICE_CONTROLS
    (void)pin;
    (void)assignment;
    (void)assignment_size;
    (void)owner;
    (void)owner_size;
    return false;
#else
    char source[SOLAR_OS_STREAM_ID_MAX];
    if (snprintf(source, sizeof(source), "adc%d", pin) >= (int)sizeof(source)) {
        return false;
    }
    const size_t count = solar_os_control_count();
    for (size_t i = 0; i < count; i++) {
        solar_os_control_info_t control;
        if (!solar_os_control_get_info(i, &control) ||
            strcmp(control.config.source, source) != 0) {
            continue;
        }
        if (assignment != NULL && assignment_size > 0U) {
            strlcpy(assignment, "ADC control", assignment_size);
        }
        if (owner != NULL && owner_size > 0U) {
            snprintf(owner, owner_size, "control:%s", control.config.name);
        }
        return true;
    }
    return false;
#endif
}

static void io_pin_summary(const solar_os_pin_info_t *pin,
                           char *assignment,
                           size_t assignment_size,
                           char *owner,
                           size_t owner_size)
{
    if (assignment_size > 0) {
        assignment[0] = '\0';
    }
    if (owner_size > 0) {
        owner[0] = '\0';
    }
    if (pin == NULL) {
        return;
    }

    solar_os_resource_claim_t claim;
    const bool claimed = solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                                       pin->pin,
                                                       -1,
                                                       &claim);
    solar_os_bus_info_t bus;
    if (claimed && strncmp(claim.owner, "bus:", 4) == 0 &&
        io_find_bus_for_pin(pin->pin, &bus, assignment, assignment_size)) {
        strlcpy(owner, claim.owner, owner_size);
        return;
    }

    solar_os_gpio_pin_info_t gpio;
    const bool has_gpio = solar_os_gpio_get_pin_info_by_pin(pin->pin, &gpio);
    if (claimed) {
#if SOLAR_OS_PACKAGE_SERVICE_PWM
        solar_os_pwm_pin_info_t pwm;
        if (strncmp(claim.owner, "pwm:", 4) == 0 &&
            io_pwm_info_for_pin(pin->pin, &pwm) && pwm.active) {
            snprintf(assignment,
                     assignment_size,
                     "PWM %" PRIu32 "Hz %u%%",
                     pwm.freq_hz,
                     (unsigned)pwm.duty_percent);
        } else
#endif
        if (has_gpio && strncmp(claim.owner, "gpio:", 5) == 0 && gpio.configured) {
            snprintf(assignment,
                     assignment_size,
                     "GPIO %s%s",
                     solar_os_gpio_mode_name(gpio.mode),
                     gpio.mode == SOLAR_OS_GPIO_MODE_OUTPUT && gpio.level_valid
                         ? (gpio.level ? " high" : " low")
                         : "");
        } else {
            strlcpy(assignment,
                    claim.label[0] != '\0' ? claim.label : "claimed",
                    assignment_size);
        }
        strlcpy(owner, claim.owner, owner_size);
        return;
    }

    if (io_control_uses_pin(pin->pin,
                            assignment,
                            assignment_size,
                            owner,
                            owner_size)) {
        return;
    }

    if (io_find_bus_for_pin(pin->pin, &bus, assignment, assignment_size)) {
        strlcpy(owner, "unclaimed", owner_size);
        return;
    }

    strlcpy(assignment,
            pin->policy == SOLAR_OS_PIN_POLICY_FIXED ? "fixed" : "free",
            assignment_size);
    strlcpy(owner, "-", owner_size);
}

static size_t io_content_rows(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    return rows > 4U ? rows - 4U : 1U;
}

static size_t io_view_count(io_view_t view)
{
    switch (view) {
    case IO_VIEW_LAYOUT:
        return solar_os_connector_pin_count();
    case IO_VIEW_PINS:
        return io_expansion_pin_count();
    case IO_VIEW_BUSES:
        return solar_os_bus_count();
    case IO_VIEW_CLAIMS:
        return solar_os_resource_claim_count();
    default:
        return 0;
    }
}

static void io_normalize_selection(void)
{
    const size_t count = io_view_count(io.view);
    if (count == 0) {
        io.selected[io.view] = 0;
    } else if (io.selected[io.view] >= count) {
        io.selected[io.view] = count - 1U;
    }
}

static size_t io_scroll_start(size_t selected, size_t count, size_t visible)
{
    if (visible == 0 || count <= visible) {
        return 0;
    }
    if (selected < visible) {
        return 0;
    }
    size_t start = selected - visible + 1U;
    if (start + visible > count) {
        start = count - visible;
    }
    return start;
}

static void io_render_title(void)
{
    const size_t cols = solar_os_tui_cols(&io.tui);
    char title[128];
    snprintf(title,
             sizeof(title),
             " Expansion I/O  [%s]  Layout | Pins | Buses | Claims",
             io_view_name(io.view));
    solar_os_tui_fill(&io.tui,
                      0,
                      0,
                      1,
                      cols,
                      ' ',
                      SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    solar_os_tui_write_cell(&io.tui, 0,
                   0,
                   cols,
                   title,
                   SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
}

static char io_layout_marker(const solar_os_connector_pin_info_t *pin)
{
    if (pin == NULL) {
        return ' ';
    }
    if (pin->kind == SOLAR_OS_CONNECTOR_PIN_POWER) {
        return '+';
    }
    if (pin->kind == SOLAR_OS_CONNECTOR_PIN_GROUND) {
        return '-';
    }
    if (pin->kind == SOLAR_OS_CONNECTOR_PIN_NC) {
        return 'x';
    }
    if (pin->kind != SOLAR_OS_CONNECTOR_PIN_GPIO || pin->pin < 0) {
        return '!';
    }

    solar_os_resource_claim_t claim;
    if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                     pin->pin,
                                     -1,
                                     &claim)) {
        return '@';
    }
    if (io_control_uses_pin(pin->pin, NULL, 0U, NULL, 0U)) {
        return '@';
    }
    solar_os_pin_info_t info;
    if (!solar_os_pin_get_info_by_pin(pin->pin, &info) ||
        info.policy == SOLAR_OS_PIN_POLICY_FIXED) {
        return '!';
    }
    return info.policy == SOLAR_OS_PIN_POLICY_RELEASABLE ? '~' : '*';
}

static bool io_selected_connector_pin(solar_os_connector_pin_info_t *pin)
{
    return solar_os_connector_pin_get_info(io.selected[IO_VIEW_LAYOUT], pin);
}

static void io_render_layout_detail(size_t row)
{
    solar_os_connector_pin_info_t connector_pin;
    char line[192] = "";
    if (!io_selected_connector_pin(&connector_pin)) {
        io_write_row(row, line, SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }

    if (connector_pin.kind == SOLAR_OS_CONNECTOR_PIN_GPIO && connector_pin.pin >= 0) {
        solar_os_pin_info_t pin;
        if (solar_os_pin_get_info_by_pin(connector_pin.pin, &pin)) {
            char assignment[48];
            char owner[32];
            io_pin_summary(&pin, assignment, sizeof(assignment), owner, sizeof(owner));
            snprintf(line,
                     sizeof(line),
                     " %s.%u GPIO%d %s  %s  owner=%s  %s",
                     connector_pin.connector,
                     (unsigned)connector_pin.position,
                     connector_pin.pin,
                     solar_os_pin_policy_name(pin.policy),
                     assignment,
                     owner,
                     pin.role != NULL ? pin.role : "");
        } else {
            snprintf(line,
                     sizeof(line),
                     " %s.%u GPIO%d %s  fixed / not runtime mapped",
                     connector_pin.connector,
                     (unsigned)connector_pin.position,
                     connector_pin.pin,
                     connector_pin.label != NULL ? connector_pin.label : "");
        }
    } else {
        snprintf(line,
                 sizeof(line),
                 " %s.%u %s  %s",
                 connector_pin.connector != NULL ? connector_pin.connector : "?",
                 (unsigned)connector_pin.position,
                 connector_pin.label != NULL ? connector_pin.label : "?",
                 solar_os_connector_pin_kind_name(connector_pin.kind));
    }
    io_write_row(row, line, SOLAR_OS_TUI_ATTR_BOLD);
}

static void io_render_layout(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    solar_os_connector_layout_info_t layout;
    solar_os_connector_pin_info_t selected;
    if (!solar_os_connector_layout_get_info(&layout) ||
        !io_selected_connector_pin(&selected)) {
        io_write_row(2, " No physical connector map for this board.", SOLAR_OS_TUI_ATTR_NORMAL);
        return;
    }

    char heading[192];
    snprintf(heading, sizeof(heading), " %s — %s", layout.title, layout.view);
    io_write_row(1, heading, SOLAR_OS_TUI_ATTR_BOLD);

    const size_t grid_rows = rows > 6U ? rows - 5U : 1U;
    const size_t visible_rows = layout.rows < grid_rows ? layout.rows : grid_rows;
    const size_t row_start = io_scroll_start(selected.row, layout.rows, visible_rows);
    size_t visible_columns = cols / 8U;
    if (visible_columns == 0) {
        visible_columns = 1;
    }
    if (visible_columns > layout.columns) {
        visible_columns = layout.columns;
    }
    const size_t column_start = io_scroll_start(selected.column,
                                                layout.columns,
                                                visible_columns);
    const size_t cell_width = cols / visible_columns;

    for (size_t screen_row = 0; screen_row < visible_rows; screen_row++) {
        const size_t physical_row = row_start + screen_row;
        const size_t tui_row = 2U + screen_row;
        solar_os_tui_fill(&io.tui,
                          tui_row,
                          0,
                          1,
                          cols,
                          ' ',
                          SOLAR_OS_TUI_ATTR_NORMAL);
        for (size_t screen_column = 0; screen_column < visible_columns; screen_column++) {
            const size_t physical_column = column_start + screen_column;
            solar_os_connector_pin_info_t pin;
            if (!solar_os_connector_pin_find(physical_row,
                                             physical_column,
                                             &pin,
                                             NULL)) {
                continue;
            }
            char cell[48];
            if (layout.columns <= 2U) {
                snprintf(cell,
                         sizeof(cell),
                         " %c%s.%u %s",
                         io_layout_marker(&pin),
                         pin.connector != NULL ? pin.connector : "?",
                         (unsigned)pin.position,
                         pin.label != NULL ? pin.label : "?");
            } else {
                snprintf(cell,
                         sizeof(cell),
                         " %c%s",
                         io_layout_marker(&pin),
                         pin.label != NULL ? pin.label : "?");
            }
            const bool is_selected = pin.row == selected.row &&
                pin.column == selected.column;
            solar_os_tui_write_cell(&io.tui, tui_row,
                           screen_column * cell_width,
                           cell_width,
                           cell,
                           is_selected ? SOLAR_OS_TUI_ATTR_INVERSE :
                                         SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }
    if (rows >= 4U) {
        io_render_layout_detail(rows - 3U);
    }
}

static void io_render_pins(void)
{
    const size_t cols = solar_os_tui_cols(&io.tui);
    const size_t count = io_expansion_pin_count();
    const size_t visible = io_content_rows();
    const size_t selected = io.selected[IO_VIEW_PINS];
    const size_t start = io_scroll_start(selected, count, visible);

    io_write_row(1, " PIN    ASSIGNMENT             OWNER              POLICY / ROLE", SOLAR_OS_TUI_ATTR_BOLD);
    for (size_t row = 0; row < visible; row++) {
        const size_t index = start + row;
        char line[192] = "";
        if (index < count) {
            solar_os_pin_info_t pin;
            char assignment[48];
            char owner[32];
            if (io_expansion_pin_get(index, &pin)) {
                io_pin_summary(&pin, assignment, sizeof(assignment), owner, sizeof(owner));
                if (cols >= 72U) {
                    snprintf(line,
                             sizeof(line),
                             " GPIO%-2d %-22s %-18s %s / %s",
                             pin.pin,
                             assignment,
                             owner,
                             solar_os_pin_policy_name(pin.policy),
                             pin.role != NULL ? pin.role : "");
                } else {
                    snprintf(line,
                             sizeof(line),
                             " GPIO%-2d %-16s %-15s %s",
                             pin.pin,
                             assignment,
                             owner,
                             solar_os_pin_policy_name(pin.policy));
                }
            }
        }
        io_write_row(2U + row,
                     line,
                     index == selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void io_bus_description(const solar_os_bus_info_t *bus, char *buffer, size_t buffer_size)
{
    if (bus == NULL || buffer_size == 0) {
        return;
    }
    switch (bus->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        snprintf(buffer,
                 buffer_size,
                 "i2c%d SDA=%d SCL=%d",
                 bus->config.i2c.port,
                 bus->config.i2c.sda_pin,
                 bus->config.i2c.scl_pin);
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        snprintf(buffer,
                 buffer_size,
                 "spi%d SCLK=%d MOSI=%d MISO=%d CS=%u",
                 bus->config.spi.host + 1,
                 bus->config.spi.sclk_pin,
                 bus->config.spi.mosi_pin,
                 bus->config.spi.miso_pin,
                 (unsigned)bus->config.spi.cs_count);
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        snprintf(buffer,
                 buffer_size,
                 "uart%d TX=%d RX=%d @%" PRIu32,
                 bus->config.uart.port,
                 bus->config.uart.tx_pin,
                 bus->config.uart.rx_pin,
                 bus->config.uart.baud_rate);
        break;
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        snprintf(buffer,
                 buffer_size,
                 "TX=%d RX=%d @%" PRIu32 " (uart%d)",
                 bus->config.uart.tx_pin,
                 bus->config.uart.rx_pin,
                 bus->config.uart.baud_rate,
                 bus->config.uart.port);
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        snprintf(buffer, buffer_size, "GPIO%d", bus->config.onewire.pin);
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        snprintf(buffer,
                 buffer_size,
                 "CLOCK=%d DATA=%d",
                 bus->config.ps2.clock_pin,
                 bus->config.ps2.data_pin);
        break;
    default:
        strlcpy(buffer, "-", buffer_size);
        break;
    }
}

static void io_render_buses(void)
{
    const size_t cols = solar_os_tui_cols(&io.tui);
    const size_t count = solar_os_bus_count();
    const size_t visible = io_content_rows();
    const size_t selected = io.selected[IO_VIEW_BUSES];
    const size_t start = io_scroll_start(selected, count, visible);

    io_write_row(1, " NAME       TYPE     ROUTING                         STATE", SOLAR_OS_TUI_ATTR_BOLD);
    for (size_t row = 0; row < visible; row++) {
        const size_t index = start + row;
        char line[192] = "";
        solar_os_bus_info_t bus;
        if (index < count && solar_os_bus_get(index, &bus)) {
            char routing[80];
            io_bus_description(&bus, routing, sizeof(routing));
            const char *state = !bus.attached ? "detached" : bus.ready ? "ready" : "attached";
            if (cols >= 72U) {
                snprintf(line,
                         sizeof(line),
                         " %-10s %-8s %-31s %s L%u",
                         bus.name,
                         solar_os_bus_protocol_name(bus.protocol),
                         routing,
                         state,
                         (unsigned)bus.lease_count);
            } else {
                snprintf(line,
                         sizeof(line),
                         " %-8s %-5s %-22s %s",
                         bus.name,
                         solar_os_bus_protocol_name(bus.protocol),
                         routing,
                         state);
            }
        }
        io_write_row(2U + row,
                     line,
                     index == selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void io_render_claims(void)
{
    const size_t count = solar_os_resource_claim_count();
    const size_t visible = io_content_rows();
    const size_t selected = io.selected[IO_VIEW_CLAIMS];
    const size_t start = io_scroll_start(selected, count, visible);

    io_write_row(1, " RESOURCE             OWNER                    LABEL", SOLAR_OS_TUI_ATTR_BOLD);
    for (size_t row = 0; row < visible; row++) {
        const size_t index = start + row;
        char line[192] = "";
        solar_os_resource_claim_t claim;
        if (index < count && solar_os_resource_get_claim(index, &claim)) {
            char resource[48];
            if (claim.secondary >= 0) {
                snprintf(resource,
                         sizeof(resource),
                         "%s:%d:%d",
                         solar_os_resource_kind_name(claim.kind),
                         claim.primary,
                         claim.secondary);
            } else {
                snprintf(resource,
                         sizeof(resource),
                         "%s:%d",
                         solar_os_resource_kind_name(claim.kind),
                         claim.primary);
            }
            snprintf(line,
                     sizeof(line),
                     " %-20s %-24s %s",
                     resource,
                     claim.owner,
                     claim.label);
        }
        io_write_row(2U + row,
                     line,
                     index == selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
}

static void io_render_browse(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    io_normalize_selection();
    solar_os_tui_clear(&io.tui);
    solar_os_tui_set_cursor_visible(&io.tui, false);
    io_render_title();
    switch (io.view) {
    case IO_VIEW_LAYOUT:
        io_render_layout();
        break;
    case IO_VIEW_PINS:
        io_render_pins();
        break;
    case IO_VIEW_BUSES:
        io_render_buses();
        break;
    case IO_VIEW_CLAIMS:
        io_render_claims();
        break;
    default:
        break;
    }
    if (rows >= 2U) {
        io_write_row(rows - 2U, io.message, SOLAR_OS_TUI_ATTR_NORMAL);
        solar_os_tui_draw_help(
            &io.tui,
            io.view == IO_VIEW_LAYOUT
                ? " Tab views  arrows move  Enter actions  N new bus  R refresh  Q exit"
                : " Tab views  arrows select  Enter actions  N new bus  R refresh  Q exit");
    }
    solar_os_tui_refresh(&io.tui);
}

static void io_action_add(io_action_kind_t kind, const char *label)
{
    if (io.action_count >= IO_ACTION_MAX) {
        return;
    }
    io.actions[io.action_count].kind = kind;
    strlcpy(io.actions[io.action_count].label,
            label != NULL ? label : "",
            sizeof(io.actions[io.action_count].label));
    io.action_count++;
}

static bool io_selected_pin_info(solar_os_pin_info_t *pin)
{
    return io_expansion_pin_get(io.selected[IO_VIEW_PINS], pin);
}

static bool io_selected_action_pin_info(solar_os_pin_info_t *pin)
{
    if (io.view == IO_VIEW_PINS) {
        return io_selected_pin_info(pin);
    }
    if (io.view == IO_VIEW_LAYOUT) {
        solar_os_connector_pin_info_t connector_pin;
        return io_selected_connector_pin(&connector_pin) &&
            connector_pin.kind == SOLAR_OS_CONNECTOR_PIN_GPIO &&
            connector_pin.pin >= 0 &&
            solar_os_pin_get_info_by_pin(connector_pin.pin, pin);
    }
    return false;
}

static bool io_selected_bus_info(solar_os_bus_info_t *bus)
{
    return solar_os_bus_get(io.selected[IO_VIEW_BUSES], bus);
}

static bool io_find_bus(const char *name, solar_os_bus_info_t *bus)
{
    if (name == NULL || bus == NULL) {
        return false;
    }
    for (size_t i = 0; i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t candidate;
        if (solar_os_bus_get(i, &candidate) && strcmp(candidate.name, name) == 0) {
            *bus = candidate;
            return true;
        }
    }
    return false;
}

static esp_err_t io_quote_shell_token(const char *value, char *quoted, size_t quoted_len)
{
    if (value == NULL || value[0] == '\0' || quoted == NULL || quoted_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool needs_quotes = false;
    size_t required = 1;
    for (const char *p = value; *p != '\0'; p++) {
        if (iscntrl((unsigned char)*p)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (isspace((unsigned char)*p)) {
            needs_quotes = true;
        }
        required++;
        if (*p == '"' || *p == '\\') {
            needs_quotes = true;
            required++;
        }
    }
    if (needs_quotes) {
        required += 2;
    }
    if (required > quoted_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!needs_quotes) {
        strlcpy(quoted, value, quoted_len);
        return ESP_OK;
    }
    char *out = quoted;
    *out++ = '"';
    for (const char *p = value; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            *out++ = '\\';
        }
        *out++ = *p;
    }
    *out++ = '"';
    *out = '\0';
    return ESP_OK;
}

static esp_err_t io_command_append(char *command, size_t command_len, const char *format, ...)
{
    const size_t used = command != NULL ? strlen(command) : 0;
    if (command == NULL || used >= command_len || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(&command[used], command_len - used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= command_len - used) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t io_bus_create_command(const solar_os_bus_info_t *bus,
                                       char *command,
                                       size_t command_len)
{
    if (bus == NULL || bus->origin != SOLAR_OS_BUS_ORIGIN_RUNTIME ||
        command == NULL || command_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[SOLAR_OS_BUS_NAME_MAX * 2 + 3];
    esp_err_t ret = io_quote_shell_token(bus->name, name, sizeof(name));
    if (ret != ESP_OK) {
        return ret;
    }
    command[0] = '\0';

    switch (bus->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        return io_command_append(command,
                                 command_len,
                                 "expansion bus create i2c %s port=i2c%d sda=gpio%d scl=gpio%d speed=%" PRIu32,
                                 name,
                                 bus->config.i2c.port,
                                 bus->config.i2c.sda_pin,
                                 bus->config.i2c.scl_pin,
                                 bus->config.i2c.speed_hz);
    case SOLAR_OS_BUS_PROTOCOL_SPI: {
        const char *host = bus->config.spi.host == SPI2_HOST ? "spi2" :
            bus->config.spi.host == SPI3_HOST ? "spi3" : NULL;
        if (host == NULL) {
            return ESP_ERR_INVALID_ARG;
        }
        char miso[12];
        if (bus->config.spi.miso_pin >= 0) {
            snprintf(miso, sizeof(miso), "gpio%d", bus->config.spi.miso_pin);
        } else {
            strlcpy(miso, "none", sizeof(miso));
        }
        ret = io_command_append(command,
                                command_len,
                                "expansion bus create spi %s host=%s sclk=gpio%d mosi=gpio%d miso=%s",
                                name,
                                host,
                                bus->config.spi.sclk_pin,
                                bus->config.spi.mosi_pin,
                                miso);
        for (size_t i = 0; ret == ESP_OK && i < bus->config.spi.cs_count; i++) {
            ret = io_command_append(command,
                                    command_len,
                                    " cs=gpio%d",
                                    bus->config.spi.cs[i].pin);
        }
        if (ret == ESP_OK) {
            ret = io_command_append(command,
                                    command_len,
                                    " max=%" PRIu32,
                                    bus->config.spi.max_transfer_size);
        }
        return ret;
    }
    case SOLAR_OS_BUS_PROTOCOL_UART:
        return io_command_append(command,
                                 command_len,
                                 "expansion bus create uart %s port=uart%d tx=gpio%d rx=gpio%d baud=%" PRIu32,
                                 name,
                                 bus->config.uart.port,
                                 bus->config.uart.tx_pin,
                                 bus->config.uart.rx_pin,
                                 bus->config.uart.baud_rate);
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        return io_command_append(command,
                                 command_len,
                                 "expansion bus create midi %s tx=gpio%d rx=gpio%d baud=%" PRIu32,
                                 name,
                                 bus->config.uart.tx_pin,
                                 bus->config.uart.rx_pin,
                                 bus->config.uart.baud_rate);
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        return io_command_append(command,
                                 command_len,
                                 "expansion bus create onewire %s pin=gpio%d",
                                 name,
                                 bus->config.onewire.pin);
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        return io_command_append(command,
                                 command_len,
                                 "expansion bus create ps2 %s clock=gpio%d data=gpio%d",
                                 name,
                                 bus->config.ps2.clock_pin,
                                 bus->config.ps2.data_pin);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t io_append_startup_command(const char *command, bool *added)
{
    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    if (added != NULL) {
        *added = false;
    }
    if (command == NULL || command[0] == '\0' || !solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = solar_os_storage_default_path(IO_STARTUP_DIR, dir, sizeof(dir));
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_storage_join_path(dir, IO_STARTUP_FILE, path, sizeof(path));
    if (ret != ESP_OK) {
        return ret;
    }
    if (solar_os_storage_mkdir(dir) != ESP_OK && errno != EEXIST) {
        return ESP_FAIL;
    }

    bool has_content = false;
    bool ends_with_newline = true;
    FILE *file = fopen(path, "r");
    if (file != NULL) {
        char line[IO_STARTUP_COMMAND_MAX + 4];
        while (fgets(line, sizeof(line), file) != NULL) {
            const size_t line_len = strlen(line);
            has_content = true;
            ends_with_newline = line_len > 0 && line[line_len - 1] == '\n';
            line[strcspn(line, "\r\n")] = '\0';
            if (strcmp(line, command) == 0) {
                fclose(file);
                return ESP_OK;
            }
        }
        if (ferror(file)) {
            fclose(file);
            return ESP_FAIL;
        }
        fclose(file);
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    file = fopen(path, "a");
    if (file == NULL) {
        return ESP_FAIL;
    }
    bool ok = true;
    if (has_content && !ends_with_newline) {
        ok = fputc('\n', file) != EOF;
    }
    if (ok) {
        ok = fprintf(file, "%s\n", command) >= 0;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    if (!ok) {
        return ESP_FAIL;
    }
    if (added != NULL) {
        *added = true;
    }
    return ESP_OK;
}

static esp_err_t io_autostart_bus(const char *name, bool *added)
{
    solar_os_bus_info_t bus;
    char command[IO_STARTUP_COMMAND_MAX];
    if (!io_find_bus(name, &bus)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = io_bus_create_command(&bus, command, sizeof(command));
    if (ret != ESP_OK) {
        return ret;
    }
    return io_append_startup_command(command, added);
}

static void io_build_bus_actions(const solar_os_bus_info_t *bus)
{
    if (bus == NULL) {
        return;
    }
    strlcpy(io.selected_bus, bus->name, sizeof(io.selected_bus));
    if (!bus->attached && bus->detachable) {
        io_action_add(IO_ACTION_BUS_ATTACH, "Attach bus");
    }
    if (bus->attached && bus->detachable) {
        io_action_add(IO_ACTION_BUS_DETACH,
                      bus->lease_count == 0 ? "Detach bus" : "Detach bus (busy)");
    }
    if (bus->protocol == SOLAR_OS_BUS_PROTOCOL_I2C) {
        io_action_add(IO_ACTION_BUS_I2C_SPEED, "Set I2C speed");
    }
    if (bus->origin == SOLAR_OS_BUS_ORIGIN_RUNTIME) {
        io_action_add(IO_ACTION_BUS_AUTOSTART, "Autostart");
        io_action_add(IO_ACTION_BUS_REMOVE,
                      bus->lease_count == 0 ? "Remove bus" : "Remove bus (busy)");
    }
}

static void io_build_free_pin_actions(const solar_os_pin_info_t *pin,
                                      const solar_os_gpio_pin_info_t *gpio)
{
    if (pin == NULL || pin->policy == SOLAR_OS_PIN_POLICY_FIXED) {
        return;
    }
    if (gpio != NULL && gpio->runtime_allowed) {
        io_action_add(IO_ACTION_GPIO_INPUT, "GPIO input");
        io_action_add(IO_ACTION_GPIO_OUTPUT_LOW, "GPIO output low");
        io_action_add(IO_ACTION_GPIO_OUTPUT_HIGH, "GPIO output high");
#if SOLAR_OS_PACKAGE_SERVICE_PWM
        if (solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM)) {
            io_action_add(IO_ACTION_PWM_START, "PWM 1 kHz / 50%");
        }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC
        if (solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) &&
            io_pin_adc_capable(pin->pin)) {
            io_action_add(IO_ACTION_ADC_READ, "Read ADC once");
        }
#endif
    }
    if (solar_os_pin_is_routable(pin->pin)) {
        io_action_add(IO_ACTION_NEW_BUS, "Create bus using this pin");
    }
}

static void io_build_actions(void)
{
    io.action_count = 0;
    io.action_selected = 0;
    io.selected_pin = -1;
    io.selected_bus[0] = '\0';

    if (io.view == IO_VIEW_BUSES) {
        solar_os_bus_info_t bus;
        if (io_selected_bus_info(&bus)) {
            io_build_bus_actions(&bus);
        }
        io_action_add(IO_ACTION_NEW_BUS, "Create new bus");
        return;
    }
    if (io.view != IO_VIEW_PINS && io.view != IO_VIEW_LAYOUT) {
        return;
    }

    solar_os_pin_info_t pin;
    if (!io_selected_action_pin_info(&pin)) {
        return;
    }
    io.selected_pin = pin.pin;

    solar_os_gpio_pin_info_t gpio;
    const bool has_gpio = solar_os_gpio_get_pin_info_by_pin(pin.pin, &gpio);
    solar_os_resource_claim_t claim;
    const bool claimed = solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN,
                                                       pin.pin,
                                                       -1,
                                                       &claim);
    solar_os_bus_info_t bus;
    char assignment[48];
    const bool routed = io_find_bus_for_pin(pin.pin, &bus, assignment, sizeof(assignment));
    if (claimed) {
        if (strncmp(claim.owner, "bus:", 4) == 0 && routed) {
            io_build_bus_actions(&bus);
        } else if (strncmp(claim.owner, "gpio:", 5) == 0) {
            io_action_add(IO_ACTION_GPIO_RELEASE, "Release GPIO");
        } else if (strncmp(claim.owner, "pwm:", 4) == 0) {
            io_action_add(IO_ACTION_PWM_STOP, "Stop PWM");
        }
        return;
    }
    if (io_control_uses_pin(pin.pin, NULL, 0U, NULL, 0U)) {
        return;
    }
    if (routed) {
        io_build_bus_actions(&bus);
    }
    io_build_free_pin_actions(&pin, has_gpio ? &gpio : NULL);
}

static void io_render_actions(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    solar_os_tui_clear(&io.tui);
    solar_os_tui_set_cursor_visible(&io.tui, false);
    io_write_row(0, " Actions", SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    if (io.action_count == 0) {
        io_write_row(2, " No available actions for this fixed or externally owned resource.", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    for (size_t i = 0; i < io.action_count && 2U + i + 1U < rows; i++) {
        char line[64];
        snprintf(line, sizeof(line), " %c %s", i == io.action_selected ? '>' : ' ', io.actions[i].label);
        io_write_row(2U + i,
                     line,
                     i == io.action_selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0) {
        solar_os_tui_draw_help(&io.tui,
                               " arrows select  Enter apply  Esc back");
    }
    solar_os_tui_refresh(&io.tui);
}

static bool io_bus_name_exists(const char *name)
{
    for (size_t i = 0; i < solar_os_bus_count(); i++) {
        solar_os_bus_info_t bus;
        if (solar_os_bus_get(i, &bus) && strcmp(bus.name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void io_default_bus_name(solar_os_bus_protocol_t protocol, char *name, size_t name_size)
{
    const char *prefix = protocol == SOLAR_OS_BUS_PROTOCOL_I2C ? "i2c" :
        protocol == SOLAR_OS_BUS_PROTOCOL_SPI ? "spi" :
        protocol == SOLAR_OS_BUS_PROTOCOL_UART ? "uart" :
        protocol == SOLAR_OS_BUS_PROTOCOL_MIDI ? "midi" :
        protocol == SOLAR_OS_BUS_PROTOCOL_PS2 ? "ps2" : "ow";
    for (unsigned i = 0; i < 10U; i++) {
        snprintf(name, name_size, "%s%u", prefix, i);
        if (!io_bus_name_exists(name)) {
            return;
        }
    }
    snprintf(name, name_size, "%snew", prefix);
}

static bool io_pin_unclaimed(int pin)
{
    solar_os_resource_claim_t claim;
    return solar_os_pin_is_routable(pin) &&
        !io_control_uses_pin(pin, NULL, 0U, NULL, 0U) &&
        !solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim);
}

static int io_nth_free_pin(size_t wanted)
{
    size_t current = 0;
    for (size_t i = 0; i < solar_os_pin_count(); i++) {
        solar_os_pin_info_t pin;
        if (!solar_os_pin_get_info(i, &pin) || !pin.expansion || !io_pin_unclaimed(pin.pin)) {
            continue;
        }
        if (current++ == wanted) {
            return pin.pin;
        }
    }
    return -1;
}

static int io_first_allowed_endpoint(solar_os_bus_protocol_t protocol)
{
    const size_t endpoint_count = solar_os_bus_runtime_endpoint_count(protocol);
    for (size_t endpoint_index = 0U; endpoint_index < endpoint_count; endpoint_index++) {
        int endpoint = -1;
        if (!solar_os_bus_runtime_endpoint_get(protocol, endpoint_index, &endpoint)) {
            continue;
        }
        if (protocol == SOLAR_OS_BUS_PROTOCOL_I2C) {
            bool used = false;
            for (size_t i = 0; i < solar_os_bus_count(); i++) {
                solar_os_bus_info_t bus;
                if (solar_os_bus_get(i, &bus) && bus.attached &&
                    bus.protocol == protocol && bus.config.i2c.port == endpoint) {
                    used = true;
                }
            }
            if (used) {
                continue;
            }
        }
        return endpoint;
    }
    return -1;
}

static void io_form_begin(solar_os_bus_protocol_t protocol)
{
    memset(&io.form, 0, sizeof(io.form));
    io.form.protocol = protocol;
    io.form.endpoint = io_first_allowed_endpoint(protocol);
    io.form.rate = protocol == SOLAR_OS_BUS_PROTOCOL_MIDI
        ? SOLAR_OS_BUS_MIDI_DEFAULT_BAUD_RATE
        : protocol == SOLAR_OS_BUS_PROTOCOL_UART
        ? SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE
        : SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ;
    io.form.max_transfer = 4096U;
    for (size_t i = 0; i < sizeof(io.form.pins) / sizeof(io.form.pins[0]); i++) {
        io.form.pins[i] = -1;
    }
    io_default_bus_name(protocol, io.form.name, sizeof(io.form.name));

    size_t required = protocol == SOLAR_OS_BUS_PROTOCOL_I2C ? 2U :
        protocol == SOLAR_OS_BUS_PROTOCOL_SPI ? 4U :
        protocol == SOLAR_OS_BUS_PROTOCOL_UART ||
            protocol == SOLAR_OS_BUS_PROTOCOL_MIDI ||
            protocol == SOLAR_OS_BUS_PROTOCOL_PS2 ? 2U : 1U;
    size_t filled = 0;
    if (io.selected_pin >= 0 && io_pin_unclaimed(io.selected_pin)) {
        io.form.pins[filled++] = io.selected_pin;
    }
    for (size_t i = 0; filled < required; i++) {
        const int pin = io_nth_free_pin(i);
        if (pin < 0) {
            break;
        }
        bool duplicate = false;
        for (size_t assigned = 0; assigned < filled; assigned++) {
            if (io.form.pins[assigned] == pin) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            io.form.pins[filled++] = pin;
        }
    }
    io.mode = IO_MODE_FORM;
    io_set_message("");
}

static void io_protocols_build(void)
{
    io.protocol_count = 0;
    if (solar_os_bus_runtime_protocol_available(SOLAR_OS_BUS_PROTOCOL_I2C)) {
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_I2C;
    }
    if (solar_os_bus_runtime_protocol_available(SOLAR_OS_BUS_PROTOCOL_SPI)) {
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_SPI;
    }
    if (solar_os_bus_runtime_protocol_available(SOLAR_OS_BUS_PROTOCOL_UART)) {
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_UART;
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_MIDI;
    }
    if (solar_os_bus_runtime_protocol_available(SOLAR_OS_BUS_PROTOCOL_ONEWIRE)) {
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_ONEWIRE;
    }
    if (solar_os_bus_runtime_protocol_available(SOLAR_OS_BUS_PROTOCOL_PS2)) {
        io.protocols[io.protocol_count++] = SOLAR_OS_BUS_PROTOCOL_PS2;
    }
    if (io.protocol_selected >= io.protocol_count) {
        io.protocol_selected = 0;
    }
}

static void io_render_protocols(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    solar_os_tui_clear(&io.tui);
    io_write_row(0, " Create named bus", SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    if (io.protocol_count == 0) {
        io_write_row(2, " No runtime-routable bus protocols on this board.", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    for (size_t i = 0; i < io.protocol_count; i++) {
        char line[48];
        snprintf(line,
                 sizeof(line),
                 " %c %s",
                 i == io.protocol_selected ? '>' : ' ',
                 solar_os_bus_protocol_name(io.protocols[i]));
        io_write_row(2U + i,
                     line,
                     i == io.protocol_selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0) {
        solar_os_tui_draw_help(&io.tui,
                               " arrows select  Enter continue  Esc back");
    }
    solar_os_tui_refresh(&io.tui);
}

static size_t io_form_field_count(void)
{
    switch (io.form.protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        return 6U;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        return 11U;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        return 6U;
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        return 5U;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        return 3U;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        return 4U;
    default:
        return 0;
    }
}

static void io_form_field(size_t index,
                          char *label,
                          size_t label_size,
                          char *value,
                          size_t value_size)
{
    label[0] = '\0';
    value[0] = '\0';
    if (index == 0) {
        strlcpy(label, "Name", label_size);
        strlcpy(value, io.form.name, value_size);
        return;
    }
    switch (io.form.protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        if (index == 1) {
            strlcpy(label, "Port", label_size);
            snprintf(value, value_size, "i2c%d", io.form.endpoint);
        } else if (index == 2 || index == 3) {
            strlcpy(label, index == 2 ? "SDA" : "SCL", label_size);
            snprintf(value, value_size, "GPIO%d", io.form.pins[index - 2U]);
        } else if (index == 4) {
            strlcpy(label, "Speed", label_size);
            snprintf(value, value_size, "%" PRIu32 " Hz", io.form.rate);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        if (index == 1) {
            strlcpy(label, "Host", label_size);
            snprintf(value, value_size, "spi%d", io.form.endpoint + 1);
        } else if (index >= 2 && index <= 8) {
            static const char *labels[] = {"SCLK", "MOSI", "MISO", "CS1", "CS2", "CS3", "CS4"};
            strlcpy(label, labels[index - 2U], label_size);
            const int pin = io.form.pins[index - 2U];
            if (pin >= 0) {
                snprintf(value, value_size, "GPIO%d", pin);
            } else {
                strlcpy(value, "none", value_size);
            }
        } else if (index == 9) {
            strlcpy(label, "Max xfer", label_size);
            snprintf(value, value_size, "%" PRIu32 " bytes", io.form.max_transfer);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        if (index == 1) {
            strlcpy(label, "Port", label_size);
            snprintf(value, value_size, "uart%d", io.form.endpoint);
        } else if (index == 2 || index == 3) {
            strlcpy(label, index == 2 ? "TX" : "RX", label_size);
            snprintf(value, value_size, "GPIO%d", io.form.pins[index - 2U]);
        } else if (index == 4) {
            strlcpy(label, "Baud", label_size);
            snprintf(value, value_size, "%" PRIu32, io.form.rate);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        if (index == 1 || index == 2) {
            strlcpy(label, index == 1 ? "TX" : "RX", label_size);
            snprintf(value, value_size, "GPIO%d", io.form.pins[index - 1U]);
        } else if (index == 3) {
            strlcpy(label, "Baud", label_size);
            snprintf(value, value_size, "%" PRIu32, io.form.rate);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        if (index == 1) {
            strlcpy(label, "Pin", label_size);
            snprintf(value, value_size, "GPIO%d", io.form.pins[0]);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        if (index == 1 || index == 2) {
            strlcpy(label, index == 1 ? "Clock" : "Data", label_size);
            snprintf(value, value_size, "GPIO%d", io.form.pins[index - 1U]);
        } else {
            strlcpy(label, "Create", label_size);
        }
        break;
    default:
        break;
    }
}

static void io_render_form(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    const size_t count = io_form_field_count();
    const size_t visible = rows > 3U ? rows - 3U : 1U;
    const size_t start = io_scroll_start(io.form.selected, count, visible);
    char title[64];
    snprintf(title, sizeof(title), " Create %s bus", solar_os_bus_protocol_name(io.form.protocol));
    solar_os_tui_clear(&io.tui);
    io_write_row(0, title, SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    for (size_t row = 0; row < visible; row++) {
        const size_t index = start + row;
        char line[128] = "";
        if (index < count) {
            char label[24];
            char value[64];
            io_form_field(index, label, sizeof(label), value, sizeof(value));
            if (value[0] != '\0') {
                snprintf(line, sizeof(line), " %-10s [ %s ]", label, value);
            } else {
                snprintf(line, sizeof(line), " [ %s ]", label);
            }
        }
        io_write_row(1U + row,
                     line,
                     index == io.form.selected ? SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows >= 2U) {
        io_write_row(rows - 2U, io.message, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (rows > 0) {
        solar_os_tui_draw_help(
            &io.tui,
                       io.form.editing_name
                           ? " type name  Enter accept  Esc cancel"
                           : " arrows fields/values  Enter edit/create  Esc back");
    }
    solar_os_tui_set_cursor_visible(&io.tui, io.form.editing_name);
    solar_os_tui_refresh(&io.tui);
}

static int io_cycle_list_value(uint32_t current,
                               int delta,
                               const uint32_t *values,
                               size_t count)
{
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i] == current) {
            index = i;
            break;
        }
    }
    index = delta < 0 ? (index == 0 ? count - 1U : index - 1U) : (index + 1U) % count;
    return (int)values[index];
}

static bool io_form_pin_used_elsewhere(int pin, size_t field)
{
    if (pin < 0) {
        return false;
    }
    for (size_t i = 0; i < sizeof(io.form.pins) / sizeof(io.form.pins[0]); i++) {
        if (i != field && io.form.pins[i] == pin) {
            return true;
        }
    }
    return false;
}

static int io_cycle_pin(int current, int delta, bool allow_none, size_t field)
{
    int pins[IO_PIN_MAX];
    size_t count = 0;
    if (allow_none) {
        pins[count++] = -1;
    }
    for (size_t i = 0; i < solar_os_pin_count() && count < IO_PIN_MAX; i++) {
        solar_os_pin_info_t pin;
        if (solar_os_pin_get_info(i, &pin) && pin.expansion &&
            io_pin_unclaimed(pin.pin) && !io_form_pin_used_elsewhere(pin.pin, field)) {
            pins[count++] = pin.pin;
        }
    }
    if (count == 0) {
        return -1;
    }
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (pins[i] == current) {
            index = i;
            break;
        }
    }
    index = delta < 0 ? (index == 0 ? count - 1U : index - 1U) : (index + 1U) % count;
    return pins[index];
}

static int io_cycle_endpoint(int current, int delta)
{
    int values[32];
    const size_t count = solar_os_bus_runtime_endpoint_count(io.form.protocol);
    if (count > sizeof(values) / sizeof(values[0])) {
        return -1;
    }
    for (size_t i = 0U; i < count; i++) {
        if (!solar_os_bus_runtime_endpoint_get(io.form.protocol, i, &values[i])) {
            return -1;
        }
    }
    if (count == 0) {
        return -1;
    }
    size_t index = 0;
    for (size_t i = 0; i < count; i++) {
        if (values[i] == current) {
            index = i;
            break;
        }
    }
    index = delta < 0 ? (index == 0 ? count - 1U : index - 1U) : (index + 1U) % count;
    return values[index];
}

static void io_form_cycle(int delta)
{
    const size_t field = io.form.selected;
    switch (io.form.protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        if (field == 1) {
            io.form.endpoint = io_cycle_endpoint(io.form.endpoint, delta);
        } else if (field == 2 || field == 3) {
            io.form.pins[field - 2U] = io_cycle_pin(io.form.pins[field - 2U],
                                                   delta,
                                                   false,
                                                   field - 2U);
        } else if (field == 4) {
            io.form.rate = (uint32_t)io_cycle_list_value(io.form.rate,
                                                               delta,
                                                               i2c_rates,
                                                               sizeof(i2c_rates) / sizeof(i2c_rates[0]));
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        if (field == 1) {
            io.form.endpoint = io_cycle_endpoint(io.form.endpoint, delta);
        } else if (field >= 2 && field <= 8) {
            const bool optional = field >= 4;
            io.form.pins[field - 2U] = io_cycle_pin(io.form.pins[field - 2U],
                                                   delta,
                                                   optional,
                                                   field - 2U);
        } else if (field == 9) {
            io.form.max_transfer = (uint32_t)io_cycle_list_value(io.form.max_transfer,
                                                                       delta,
                                                                       spi_sizes,
                                                                       sizeof(spi_sizes) / sizeof(spi_sizes[0]));
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        if (field == 1) {
            io.form.endpoint = io_cycle_endpoint(io.form.endpoint, delta);
        } else if (field == 2 || field == 3) {
            io.form.pins[field - 2U] = io_cycle_pin(io.form.pins[field - 2U],
                                                   delta,
                                                   false,
                                                   field - 2U);
        } else if (field == 4) {
            io.form.rate = (uint32_t)io_cycle_list_value(io.form.rate,
                                                               delta,
                                                               uart_rates,
                                                               sizeof(uart_rates) / sizeof(uart_rates[0]));
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        if (field == 1 || field == 2) {
            io.form.pins[field - 1U] = io_cycle_pin(io.form.pins[field - 1U],
                                                   delta,
                                                   false,
                                                   field - 1U);
        } else if (field == 3) {
            io.form.rate = (uint32_t)io_cycle_list_value(
                io.form.rate,
                delta,
                midi_rates,
                sizeof(midi_rates) / sizeof(midi_rates[0]));
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        if (field == 1) {
            io.form.pins[0] = io_cycle_pin(io.form.pins[0], delta, false, 0);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        if (field == 1 || field == 2) {
            io.form.pins[field - 1U] = io_cycle_pin(io.form.pins[field - 1U],
                                                   delta,
                                                   false,
                                                   field - 1U);
        }
        break;
    default:
        break;
    }
}

static esp_err_t io_form_create(void)
{
    solar_os_bus_definition_t definition = {
        .name = io.form.name,
        .protocol = io.form.protocol,
        .origin = SOLAR_OS_BUS_ORIGIN_RUNTIME,
        .sharing = io.form.protocol == SOLAR_OS_BUS_PROTOCOL_UART ||
                io.form.protocol == SOLAR_OS_BUS_PROTOCOL_MIDI ||
                io.form.protocol == SOLAR_OS_BUS_PROTOCOL_ONEWIRE ||
                io.form.protocol == SOLAR_OS_BUS_PROTOCOL_PS2
            ? SOLAR_OS_BUS_EXCLUSIVE
            : SOLAR_OS_BUS_SHARED,
    };
    switch (io.form.protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C:
        definition.config.i2c = (solar_os_bus_i2c_config_t) {
            .port = io.form.endpoint,
            .sda_pin = io.form.pins[0],
            .scl_pin = io.form.pins[1],
            .speed_hz = io.form.rate,
        };
        break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        definition.config.spi = (solar_os_bus_spi_config_t) {
            .host = io.form.endpoint,
            .sclk_pin = io.form.pins[0],
            .mosi_pin = io.form.pins[1],
            .miso_pin = io.form.pins[2],
            .max_transfer_size = io.form.max_transfer,
        };
        for (size_t i = 3; i < 7 && definition.config.spi.cs_count < IO_FORM_CS_MAX; i++) {
            if (io.form.pins[i] < 0) {
                continue;
            }
            solar_os_bus_pin_t *cs = &definition.config.spi.cs[definition.config.spi.cs_count++];
            cs->pin = io.form.pins[i];
            snprintf(cs->name, sizeof(cs->name), "gpio%d", cs->pin);
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART:
        definition.config.uart = (solar_os_bus_uart_config_t) {
            .port = io.form.endpoint,
            .tx_pin = io.form.pins[0],
            .rx_pin = io.form.pins[1],
            .baud_rate = io.form.rate,
        };
        break;
    case SOLAR_OS_BUS_PROTOCOL_MIDI:
        definition.config.uart = (solar_os_bus_uart_config_t) {
            .port = -1,
            .tx_pin = io.form.pins[0],
            .rx_pin = io.form.pins[1],
            .baud_rate = io.form.rate,
        };
        break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE:
        definition.config.onewire.pin = io.form.pins[0];
        break;
    case SOLAR_OS_BUS_PROTOCOL_PS2:
        definition.config.ps2 = (solar_os_bus_ps2_config_t) {
            .clock_pin = io.form.pins[0],
            .data_pin = io.form.pins[1],
        };
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return solar_os_bus_register(&definition);
}

static void io_render_confirm(void)
{
    const size_t rows = solar_os_tui_rows(&io.tui);
    const size_t cols = solar_os_tui_cols(&io.tui);
    char prompt[96];
    solar_os_tui_clear(&io.tui);
    io_write_row(0, " Confirm", SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
    snprintf(prompt, sizeof(prompt), " Remove runtime bus '%s'?", io.selected_bus);
    io_write_row(2, prompt, SOLAR_OS_TUI_ATTR_BOLD);
    io_write_row(4, " This releases its controller and signal pins.", SOLAR_OS_TUI_ATTR_NORMAL);
    if (rows > 0) {
        solar_os_tui_draw_help(&io.tui, " Y remove  N/Esc cancel");
    }
    solar_os_tui_refresh(&io.tui);
}

static void io_render(void)
{
    switch (io.mode) {
    case IO_MODE_BROWSE:
        io_render_browse();
        break;
    case IO_MODE_ACTIONS:
        io_render_actions();
        break;
    case IO_MODE_PROTOCOL:
        io_render_protocols();
        break;
    case IO_MODE_FORM:
        io_render_form();
        break;
    case IO_MODE_I2C_SPEED: {
        const size_t rows = solar_os_tui_rows(&io.tui);
        const size_t cols = solar_os_tui_cols(&io.tui);
        solar_os_tui_clear(&io.tui);
        solar_os_tui_set_cursor_visible(&io.tui, false);
        char heading[64];
        snprintf(heading, sizeof(heading), " I2C speed - %s", io.selected_bus);
        io_write_row(0, heading,
                     SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD);
        for (size_t i = 0; i < sizeof(i2c_rates) / sizeof(i2c_rates[0]) &&
                           2U + i + 1U < rows; i++) {
            char line[40];
            snprintf(line, sizeof(line), " %c %" PRIu32 " Hz",
                     i == io.i2c_rate_selected ? '>' : ' ', i2c_rates[i]);
            io_write_row(2U + i, line,
                         i == io.i2c_rate_selected ? SOLAR_OS_TUI_ATTR_INVERSE :
                                                    SOLAR_OS_TUI_ATTR_NORMAL);
        }
        if (rows > 0U) {
            solar_os_tui_draw_help(&io.tui,
                                   " arrows select  Enter apply  Esc back");
        }
        solar_os_tui_refresh(&io.tui);
        break;
    }
    case IO_MODE_CONFIRM:
        io_render_confirm();
        break;
    default:
        break;
    }
}

static void io_start_protocol_picker(void)
{
    io_protocols_build();
    io.mode = IO_MODE_PROTOCOL;
    io_set_message("");
}

static void io_execute_action(io_action_kind_t action)
{
    esp_err_t err = ESP_OK;
    switch (action) {
    case IO_ACTION_GPIO_INPUT:
        err = solar_os_gpio_configure(io.selected_pin,
                                      SOLAR_OS_GPIO_MODE_INPUT,
                                      SOLAR_OS_GPIO_PULL_NONE);
        break;
    case IO_ACTION_GPIO_OUTPUT_LOW:
    case IO_ACTION_GPIO_OUTPUT_HIGH:
        err = solar_os_gpio_configure(io.selected_pin,
                                      SOLAR_OS_GPIO_MODE_OUTPUT,
                                      SOLAR_OS_GPIO_PULL_NONE);
        if (err == ESP_OK) {
            err = solar_os_gpio_write(io.selected_pin, action == IO_ACTION_GPIO_OUTPUT_HIGH);
        }
        break;
    case IO_ACTION_GPIO_RELEASE:
        err = solar_os_gpio_release(io.selected_pin);
        break;
#if SOLAR_OS_PACKAGE_SERVICE_PWM
    case IO_ACTION_PWM_START:
        err = solar_os_pwm_set(io.selected_pin, 1000U, 50U);
        break;
    case IO_ACTION_PWM_STOP:
        err = solar_os_pwm_stop(io.selected_pin);
        break;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC
    case IO_ACTION_ADC_READ: {
        solar_os_adc_sample_t sample;
        err = solar_os_adc_read(io.selected_pin, &sample);
        if (err == ESP_OK) {
            snprintf(io.message,
                     sizeof(io.message),
                     "GPIO%d ADC%d:%d raw=%d %umV%s",
                     sample.pin,
                     sample.unit,
                     sample.channel,
                     sample.raw,
                     (unsigned)sample.voltage_mv,
                     sample.calibrated ? " calibrated" : "");
        }
        break;
    }
#endif
    case IO_ACTION_BUS_ATTACH:
        err = solar_os_bus_attach(io.selected_bus);
        break;
    case IO_ACTION_BUS_DETACH:
        err = solar_os_bus_detach(io.selected_bus);
        break;
    case IO_ACTION_BUS_I2C_SPEED: {
        solar_os_bus_info_t bus;
        if (!solar_os_bus_find(io.selected_bus, SOLAR_OS_BUS_PROTOCOL_I2C, &bus)) {
            err = ESP_ERR_NOT_FOUND;
            break;
        }
        io.i2c_rate_selected = 0U;
        uint32_t best_distance = UINT32_MAX;
        for (size_t i = 0; i < sizeof(i2c_rates) / sizeof(i2c_rates[0]); i++) {
            const uint32_t distance = i2c_rates[i] > bus.config.i2c.speed_hz
                ? i2c_rates[i] - bus.config.i2c.speed_hz
                : bus.config.i2c.speed_hz - i2c_rates[i];
            if (distance < best_distance) {
                best_distance = distance;
                io.i2c_rate_selected = i;
            }
        }
        io.mode = IO_MODE_I2C_SPEED;
        io_render();
        return;
    }
    case IO_ACTION_BUS_AUTOSTART: {
        bool added = false;
        err = io_autostart_bus(io.selected_bus, &added);
        if (err == ESP_OK) {
            io_set_message(added ? "autostart added to /.shell/startup" :
                                   "autostart already in /.shell/startup");
        }
        break;
    }
    case IO_ACTION_BUS_REMOVE:
        io.confirm_action = action;
        io.mode = IO_MODE_CONFIRM;
        io_render();
        return;
    case IO_ACTION_NEW_BUS:
        io_start_protocol_picker();
        io_render();
        return;
    default:
        return;
    }
    if (err == ESP_OK && action != IO_ACTION_ADC_READ &&
        action != IO_ACTION_BUS_AUTOSTART) {
        io_set_message("assignment updated");
    } else if (err != ESP_OK) {
        io_set_error("update", err);
    }
    io.mode = IO_MODE_BROWSE;
    io_render();
}

static void io_move_selection(int delta)
{
    const size_t count = io_view_count(io.view);
    size_t *selected = &io.selected[io.view];
    if (count == 0) {
        *selected = 0;
        return;
    }
    if (delta < 0) {
        const size_t amount = (size_t)(-delta);
        *selected = amount > *selected ? 0 : *selected - amount;
    } else {
        *selected += (size_t)delta;
        if (*selected >= count) {
            *selected = count - 1U;
        }
    }
}

static void io_move_layout(int row_delta, int column_delta)
{
    solar_os_connector_layout_info_t layout;
    solar_os_connector_pin_info_t current;
    if (!solar_os_connector_layout_get_info(&layout) ||
        !io_selected_connector_pin(&current)) {
        return;
    }

    int row = (int)current.row + row_delta;
    int column = (int)current.column + column_delta;
    if (row < 0) row = 0;
    if (column < 0) column = 0;
    if ((size_t)row >= layout.rows) row = (int)layout.rows - 1;
    if ((size_t)column >= layout.columns) column = (int)layout.columns - 1;

    size_t index = 0;
    if (solar_os_connector_pin_find((size_t)row,
                                    (size_t)column,
                                    NULL,
                                    &index)) {
        io.selected[IO_VIEW_LAYOUT] = index;
    }
}

static void io_handle_browse_key(uint8_t key)
{
    switch (key) {
    case SOLAR_OS_KEY_ESCAPE:
    case SOLAR_OS_KEY_APP_EXIT:
    case 'q':
    case 'Q':
        solar_os_context_finish(io.ctx, 0, NULL);
        return;
    case '\t':
        io.view = (io_view_t)((io.view + 1U) % IO_VIEW_COUNT);
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout(0, 1);
        } else {
            io.view = (io_view_t)((io.view + 1U) % IO_VIEW_COUNT);
        }
        break;
    case SOLAR_OS_KEY_LEFT:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout(0, -1);
        } else {
            io.view = io.view == 0 ? IO_VIEW_COUNT - 1 : (io_view_t)(io.view - 1);
        }
        break;
    case SOLAR_OS_KEY_UP:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout(-1, 0);
        } else {
            io_move_selection(-1);
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout(1, 0);
        } else {
            io_move_selection(1);
        }
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout(-(int)io_content_rows(), 0);
        } else {
            io_move_selection(-(int)io_content_rows());
        }
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        if (io.view == IO_VIEW_LAYOUT) {
            io_move_layout((int)io_content_rows(), 0);
        } else {
            io_move_selection((int)io_content_rows());
        }
        break;
    case SOLAR_OS_KEY_HOME:
        io.selected[io.view] = 0;
        break;
    case SOLAR_OS_KEY_END: {
        const size_t count = io_view_count(io.view);
        io.selected[io.view] = count > 0 ? count - 1U : 0;
        break;
    }
    case '\r':
    case '\n':
        io_build_actions();
        io.mode = IO_MODE_ACTIONS;
        break;
    case 'n':
    case 'N': {
        io.selected_pin = -1;
        solar_os_pin_info_t pin;
        if ((io.view == IO_VIEW_PINS || io.view == IO_VIEW_LAYOUT) &&
            io_selected_action_pin_info(&pin) &&
            io_pin_unclaimed(pin.pin)) {
            io.selected_pin = pin.pin;
        }
        io_start_protocol_picker();
        break;
    }
    case 'r':
    case 'R':
        io_set_message("refreshed");
        break;
    default:
        return;
    }
    io_render();
}

static void io_handle_actions_key(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_LEFT) {
        io.mode = IO_MODE_BROWSE;
        io_render();
        return;
    }
    if (key == SOLAR_OS_KEY_UP && io.action_selected > 0) {
        io.action_selected--;
    } else if (key == SOLAR_OS_KEY_DOWN && io.action_selected + 1U < io.action_count) {
        io.action_selected++;
    } else if ((key == '\r' || key == '\n') && io.action_selected < io.action_count) {
        io_execute_action(io.actions[io.action_selected].kind);
        return;
    }
    io_render();
}

static void io_handle_protocol_key(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_LEFT) {
        io.mode = IO_MODE_BROWSE;
    } else if (key == SOLAR_OS_KEY_UP && io.protocol_selected > 0) {
        io.protocol_selected--;
    } else if (key == SOLAR_OS_KEY_DOWN && io.protocol_selected + 1U < io.protocol_count) {
        io.protocol_selected++;
    } else if ((key == '\r' || key == '\n' || key == SOLAR_OS_KEY_RIGHT) &&
               io.protocol_selected < io.protocol_count) {
        io_form_begin(io.protocols[io.protocol_selected]);
    }
    io_render();
}

static void io_form_edit_name(uint8_t key)
{
    const size_t len = strlen(io.form.name);
    if (key == SOLAR_OS_KEY_ESCAPE) {
        strlcpy(io.form.name, io.form.original_name, sizeof(io.form.name));
        io.form.editing_name = false;
    } else if (key == '\r' || key == '\n') {
        io.form.editing_name = false;
    } else if ((key == '\b' || key == 0x7fU || key == SOLAR_OS_KEY_DELETE) && len > 0) {
        io.form.name[len - 1U] = '\0';
    } else if (isprint(key) && len + 1U < sizeof(io.form.name)) {
        io.form.name[len] = (char)key;
        io.form.name[len + 1U] = '\0';
    }
    io_render();
}

static void io_handle_form_key(uint8_t key)
{
    if (io.form.editing_name) {
        io_form_edit_name(key);
        return;
    }
    const size_t count = io_form_field_count();
    if (key == SOLAR_OS_KEY_ESCAPE) {
        io.mode = IO_MODE_PROTOCOL;
    } else if (key == SOLAR_OS_KEY_UP && io.form.selected > 0) {
        io.form.selected--;
    } else if (key == SOLAR_OS_KEY_DOWN && io.form.selected + 1U < count) {
        io.form.selected++;
    } else if (key == SOLAR_OS_KEY_LEFT) {
        io_form_cycle(-1);
    } else if (key == SOLAR_OS_KEY_RIGHT) {
        io_form_cycle(1);
    } else if (key == '\r' || key == '\n') {
        if (io.form.selected == 0) {
            strlcpy(io.form.original_name, io.form.name, sizeof(io.form.original_name));
            io.form.editing_name = true;
        } else if (io.form.selected + 1U == count) {
            const esp_err_t err = io_form_create();
            if (err == ESP_OK) {
                io.view = IO_VIEW_BUSES;
                io.selected[IO_VIEW_BUSES] = solar_os_bus_count() > 0 ? solar_os_bus_count() - 1U : 0;
                io.mode = IO_MODE_BROWSE;
                io_set_message("bus created");
            } else {
                io_set_error("create", err);
            }
        } else {
            io_form_cycle(1);
        }
    }
    io_render();
}

static void io_handle_i2c_speed_key(uint8_t key)
{
    const size_t count = sizeof(i2c_rates) / sizeof(i2c_rates[0]);
    if (key == SOLAR_OS_KEY_ESCAPE) {
        io.mode = IO_MODE_ACTIONS;
    } else if ((key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_LEFT) &&
               io.i2c_rate_selected > 0U) {
        io.i2c_rate_selected--;
    } else if ((key == SOLAR_OS_KEY_DOWN || key == SOLAR_OS_KEY_RIGHT) &&
               io.i2c_rate_selected + 1U < count) {
        io.i2c_rate_selected++;
    } else if (key == '\r' || key == '\n') {
        const uint32_t speed_hz = i2c_rates[io.i2c_rate_selected];
        const esp_err_t err = solar_os_bus_i2c_set_speed(io.selected_bus,
                                                         speed_hz);
        io.mode = IO_MODE_BROWSE;
        if (err == ESP_OK) {
            snprintf(io.message, sizeof(io.message), "%s speed set to %" PRIu32 " Hz",
                     io.selected_bus, speed_hz);
        } else {
            io_set_error("I2C speed", err);
        }
    }
    io_render();
}

static void io_handle_confirm_key(uint8_t key)
{
    if (key == 'y' || key == 'Y') {
        const esp_err_t err = solar_os_bus_unregister(io.selected_bus);
        io.mode = IO_MODE_BROWSE;
        if (err == ESP_OK) {
            const size_t count = solar_os_bus_count();
            if (io.selected[IO_VIEW_BUSES] >= count && count > 0) {
                io.selected[IO_VIEW_BUSES] = count - 1U;
            }
            io_set_message("bus removed");
        } else {
            io_set_error("remove", err);
        }
    } else if (key == 'n' || key == 'N' || key == SOLAR_OS_KEY_ESCAPE) {
        io.mode = IO_MODE_BROWSE;
        io_set_message("cancelled");
    }
    io_render();
}

static esp_err_t io_start(solar_os_context_t *ctx)
{
    memset(&io, 0, sizeof(io));
    io.ctx = ctx;
    io.selected_pin = -1;
    esp_err_t err = solar_os_buses_init();
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_resources_init();
    if (err != ESP_OK) {
        return err;
    }
    err = solar_os_tui_screen_begin(&io.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_tui_set_cursor_visible(&io.tui, false);
    io_render();
    return ESP_OK;
}

static void io_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&io.tui, true);
    solar_os_tui_refresh(&io.tui);
}

static void io_resume(solar_os_context_t *ctx)
{
    io.ctx = ctx;
    solar_os_tui_set_cursor_visible(&io.tui, false);
    io_render();
}

static void io_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&io.tui, true);
    solar_os_tui_clear(&io.tui);
    solar_os_tui_refresh(&io.tui);
    solar_os_tui_end(&io.tui);
    memset(&io, 0, sizeof(io));
}

static bool io_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        io_render();
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }
    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(io.ctx, 0, NULL);
        return true;
    }
    switch (io.mode) {
    case IO_MODE_BROWSE:
        io_handle_browse_key(key);
        break;
    case IO_MODE_ACTIONS:
        io_handle_actions_key(key);
        break;
    case IO_MODE_PROTOCOL:
        io_handle_protocol_key(key);
        break;
    case IO_MODE_FORM:
        io_handle_form_key(key);
        break;
    case IO_MODE_I2C_SPEED:
        io_handle_i2c_speed_key(key);
        break;
    case IO_MODE_CONFIRM:
        io_handle_confirm_key(key);
        break;
    default:
        break;
    }
    return true;
}

static void io_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Expansion I/O - %s", io_view_name(io.view));
}

const solar_os_app_t solar_os_io_app = {
    .name = "io",
    .summary = "expansion pin and bus manager",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = io_start,
    .suspend = io_suspend,
    .resume = io_resume,
    .stop = io_stop,
    .event = io_event,
    .title = io_title,
    .state_slot = &io_state,
    .state_size = sizeof(io_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};
