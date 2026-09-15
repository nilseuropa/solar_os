#include "solar_os_shell_tui_apps.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_buses.h"
#include "solar_os_expansion.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_keys.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_expansion_internal.h"
#include "solar_os_stream.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define EXPANSION_TUI_MESSAGE_MAX 96
#define EXPANSION_TUI_VALUE_MAX 32
#define EXPANSION_TUI_MANUAL_MAX 192
#define EXPANSION_TUI_FORM_SPEC_MAX 24

typedef enum {
    EXPANSION_TUI_VIEW_DEVICES,
    EXPANSION_TUI_VIEW_DEVICE_DETAIL,
    EXPANSION_TUI_VIEW_CATEGORIES,
    EXPANSION_TUI_VIEW_DRIVERS,
    EXPANSION_TUI_VIEW_DRIVER_DETAIL,
    EXPANSION_TUI_VIEW_ATTACH,
    EXPANSION_TUI_VIEW_DETACH_CONFIRM,
} expansion_tui_view_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    expansion_tui_view_t view;
    solar_os_tui_viewport_t devices;
    solar_os_tui_viewport_t categories;
    solar_os_tui_viewport_t drivers;
    solar_os_tui_viewport_t detail;
    solar_os_tui_viewport_t form;
    solar_os_expansion_category_t category;
    solar_os_expansion_device_t device;
    solar_os_expansion_driver_t driver;
    char message[EXPANSION_TUI_MESSAGE_MAX];
    char device_name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char values[EXPANSION_TUI_FORM_SPEC_MAX][EXPANSION_TUI_VALUE_MAX];
    char manual_resources[EXPANSION_TUI_MANUAL_MAX];
    solar_os_tui_input_state_t input;
    bool editing;
} expansion_tui_state_t;

static void *expansion_tui_state;
#define expansion_tui (*(expansion_tui_state_t *)expansion_tui_state)

static void expansion_tui_render(void);

static size_t expansion_tui_body_rows(void)
{
    return solar_os_tui_screen_content_rows(&expansion_tui.tui, 2U, 1U);
}

static void expansion_tui_set_message(const char *message)
{
    strlcpy(expansion_tui.message,
            message != NULL ? message : "",
            sizeof(expansion_tui.message));
}

static void expansion_tui_set_error(const char *operation, esp_err_t err)
{
    snprintf(expansion_tui.message,
             sizeof(expansion_tui.message),
             "%s: %s",
             operation,
             solar_os_shell_error_text(err));
}

static void expansion_tui_draw_tabs(bool devices_selected)
{
    const size_t cols = solar_os_tui_cols(&expansion_tui.tui);
    if (solar_os_tui_rows(&expansion_tui.tui) < 2U || cols == 0U) {
        return;
    }
    const size_t split = cols / 2U;
    solar_os_tui_draw_tab(&expansion_tui.tui,
                          1U,
                          0U,
                          split,
                          "Devices",
                          devices_selected);
    solar_os_tui_draw_tab(&expansion_tui.tui,
                          1U,
                          split,
                          cols - split,
                          "Drivers",
                          !devices_selected);
}

static void expansion_tui_finish_render(bool cursor_visible)
{
    solar_os_tui_set_cursor_visible(&expansion_tui.tui, cursor_visible);
    solar_os_tui_refresh(&expansion_tui.tui);
}

static void expansion_tui_draw_help(const char *help)
{
    solar_os_tui_draw_footer(&expansion_tui.tui,
                             expansion_tui.message,
                             help);
}

static size_t expansion_tui_category_driver_count(
    solar_os_expansion_category_t category)
{
    size_t count = 0U;
    for (size_t i = 0U; i < solar_os_expansion_driver_count(); i++) {
        solar_os_expansion_driver_t driver;
        if (solar_os_expansion_get_driver(i, &driver) &&
            driver.category == category) {
            count++;
        }
    }
    return count;
}

static size_t expansion_tui_category_count(void)
{
    size_t count = 0U;
    for (solar_os_expansion_category_t category = SOLAR_OS_EXPANSION_CATEGORY_AUDIO;
         category < SOLAR_OS_EXPANSION_CATEGORY_COUNT;
         category++) {
        if (expansion_tui_category_driver_count(category) > 0U) {
            count++;
        }
    }
    return count;
}

static bool expansion_tui_get_category(size_t index,
                                       solar_os_expansion_category_t *category)
{
    size_t current = 0U;
    for (solar_os_expansion_category_t candidate = SOLAR_OS_EXPANSION_CATEGORY_AUDIO;
         candidate < SOLAR_OS_EXPANSION_CATEGORY_COUNT;
         candidate++) {
        if (expansion_tui_category_driver_count(candidate) == 0U) {
            continue;
        }
        if (current++ == index) {
            if (category != NULL) {
                *category = candidate;
            }
            return true;
        }
    }
    return false;
}

static bool expansion_tui_get_driver(solar_os_expansion_category_t category,
                                     size_t index,
                                     solar_os_expansion_driver_t *driver)
{
    char after[SOLAR_OS_EXPANSION_DRIVER_NAME_MAX] = "";
    bool have_after = false;
    solar_os_expansion_driver_t selected;

    for (size_t position = 0U; position <= index; position++) {
        bool found = false;
        for (size_t i = 0U; i < solar_os_expansion_driver_count(); i++) {
            solar_os_expansion_driver_t candidate;
            if (!solar_os_expansion_get_driver(i, &candidate) ||
                candidate.category != category ||
                (have_after && strcmp(candidate.name, after) <= 0)) {
                continue;
            }
            if (!found || strcmp(candidate.name, selected.name) < 0) {
                selected = candidate;
                found = true;
            }
        }
        if (!found) {
            return false;
        }
        strlcpy(after, selected.name, sizeof(after));
        have_after = true;
    }
    if (driver != NULL) {
        *driver = selected;
    }
    return true;
}

static void expansion_tui_render_devices(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = solar_os_expansion_device_count();
    solar_os_tui_clear(tui);

    char detail[24];
    snprintf(detail, sizeof(detail), "%u attached", (unsigned)count);
    solar_os_tui_draw_title(tui, "expansion", detail);
    expansion_tui_draw_tabs(true);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.devices, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.devices.top + row;
        solar_os_expansion_device_t device;
        if (index >= count || !solar_os_expansion_get_device(index, &device)) {
            break;
        }
        char line[EXPANSION_TUI_MESSAGE_MAX];
        snprintf(line,
                 sizeof(line),
                 "%c %-19s  %-19s  %s",
                 device.ready ? '*' : '-',
                 device.name,
                 device.driver,
                 device.detachable ? "detachable" : "fixed");
        solar_os_tui_write_cell(tui,
                                2U + row,
                                0U,
                                cols,
                                line,
                                index == expansion_tui.devices.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (count == 0U && visible > 0U) {
        solar_os_tui_write_cell(tui,
                                2U,
                                0U,
                                cols,
                                "no expansion devices attached",
                                SOLAR_OS_TUI_ATTR_NORMAL);
    }
    expansion_tui_draw_help("arrows select  enter details  N attach  tab drivers  Q exit");
    expansion_tui_finish_render(false);
}

static void expansion_tui_format_binding(
    const solar_os_expansion_binding_t *binding,
    char *line,
    size_t line_len)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        snprintf(line,
                 line_len,
                 "%s:%s = GPIO%d",
                 solar_os_expansion_binding_kind_name(binding->kind),
                 binding->role,
                 binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE:
        if (binding->target[0] == '\0') {
            snprintf(line,
                     line_len,
                     "gpio_line:%s = GPIO%d",
                     binding->role,
                     binding->value);
        } else {
            snprintf(line,
                     line_len,
                     "gpio_line:%s = %s:%d",
                     binding->role,
                     binding->target,
                     binding->value);
        }
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        snprintf(line, line_len, "i2s = i2s%d", binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        snprintf(line,
                 line_len,
                 "%s = %s",
                 solar_os_expansion_binding_kind_name(binding->kind),
                 binding->target);
        break;
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        snprintf(line, line_len, "%s = %s", binding->role, binding->target);
        break;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        snprintf(line, line_len, "address = 0x%02x", binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        snprintf(line,
                 line_len,
                 "cs:%s = GPIO%d",
                 binding->target,
                 binding->value);
        break;
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        snprintf(line, line_len, "%s = %d", binding->role, binding->value);
        break;
    default:
        strlcpy(line, "unknown binding", line_len);
        break;
    }
}

static size_t expansion_tui_device_detail_count(void)
{
    return 6U + expansion_tui.device.binding_count;
}

static void expansion_tui_device_detail_line(size_t index,
                                             char *line,
                                             size_t line_len)
{
    switch (index) {
    case 0U:
        snprintf(line, line_len, "driver: %s", expansion_tui.device.driver);
        break;
    case 1U:
        snprintf(line,
                 line_len,
                 "origin: %s",
                 solar_os_expansion_origin_name(expansion_tui.device.origin));
        break;
    case 2U:
        snprintf(line,
                 line_len,
                 "state: %s",
                 expansion_tui.device.ready ? "ready" :
                     expansion_tui.device.active ? "active" : "inactive");
        break;
    case 3U:
        snprintf(line,
                 line_len,
                 "startup: %s",
                 expansion_tui.device.autostart ? "automatic" : "manual");
        break;
    case 4U:
        snprintf(line,
                 line_len,
                 "attachment: %s",
                 expansion_tui.device.detachable ? "detachable" : "fixed");
        break;
    case 5U:
        snprintf(line,
                 line_len,
                 "bindings: %u",
                 (unsigned)expansion_tui.device.binding_count);
        break;
    default:
        expansion_tui_format_binding(&expansion_tui.device.bindings[index - 6U],
                                     line,
                                     line_len);
        break;
    }
}

static void expansion_tui_render_device_detail(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = expansion_tui_device_detail_count();
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, "device", expansion_tui.device.name);
    expansion_tui_draw_tabs(true);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.detail, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.detail.top + row;
        if (index >= count) {
            break;
        }
        char line[EXPANSION_TUI_MESSAGE_MAX];
        expansion_tui_device_detail_line(index, line, sizeof(line));
        solar_os_tui_write_cell(tui, 2U + row, 0U, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    expansion_tui_draw_help(expansion_tui.device.detachable ?
                                "arrows scroll  D/del detach  esc back" :
                                "arrows scroll  fixed device  esc back");
    expansion_tui_finish_render(false);
}

static solar_os_tui_rect_t expansion_tui_popup_bounds(void)
{
    const size_t rows = solar_os_tui_rows(&expansion_tui.tui);
    const size_t height = solar_os_tui_screen_content_rows(
        &expansion_tui.tui, 1U, 1U);
    return (solar_os_tui_rect_t) {
        .row = rows > 2U ? 1U : 0U,
        .col = 0U,
        .height = rows > 2U ? height : rows,
        .width = solar_os_tui_cols(&expansion_tui.tui),
    };
}

static void expansion_tui_render_detach_confirm(void)
{
    expansion_tui_render_device_detail();
    char message[EXPANSION_TUI_MESSAGE_MAX];
    snprintf(message,
             sizeof(message),
             "Detach %s?\ny/N",
             expansion_tui.device.name);
    const solar_os_tui_rect_t bounds = expansion_tui_popup_bounds();
    solar_os_tui_text_popup(&expansion_tui.tui,
                            &bounds,
                            "Detach device",
                            message,
                            NULL);
    expansion_tui_finish_render(false);
}

static void expansion_tui_render_categories(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = expansion_tui_category_count();
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, "expansion", "driver categories");
    expansion_tui_draw_tabs(false);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.categories, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.categories.top + row;
        solar_os_expansion_category_t category;
        if (!expansion_tui_get_category(index, &category)) {
            break;
        }
        char line[48];
        snprintf(line,
                 sizeof(line),
                 "%-16s %u driver%s",
                 solar_os_expansion_category_name(category),
                 (unsigned)expansion_tui_category_driver_count(category),
                 expansion_tui_category_driver_count(category) == 1U ? "" : "s");
        solar_os_tui_write_cell(tui,
                                2U + row,
                                0U,
                                cols,
                                line,
                                index == expansion_tui.categories.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
    }
    expansion_tui_draw_help("arrows select  enter browse  tab devices  Q exit");
    expansion_tui_finish_render(false);
}

static void expansion_tui_render_drivers(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = expansion_tui_category_driver_count(expansion_tui.category);
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui,
                            "drivers",
                            solar_os_expansion_category_name(expansion_tui.category));
    expansion_tui_draw_tabs(false);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.drivers, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.drivers.top + row;
        solar_os_expansion_driver_t driver;
        if (!expansion_tui_get_driver(expansion_tui.category, index, &driver)) {
            break;
        }
        char line[EXPANSION_TUI_MESSAGE_MAX];
        snprintf(line,
                 sizeof(line),
                 "%c %-20s %s",
                 solar_os_expansion_driver_supported(driver.name) ? '*' : '-',
                 driver.name,
                 driver.summary != NULL ? driver.summary : "");
        solar_os_tui_write_cell(tui,
                                2U + row,
                                0U,
                                cols,
                                line,
                                index == expansion_tui.drivers.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
    }
    expansion_tui_draw_help("arrows select  enter details  esc categories");
    expansion_tui_finish_render(false);
}

static size_t expansion_tui_driver_detail_count(void)
{
    return 5U + (expansion_tui.driver.allow_unlisted_bindings ?
                     1U : expansion_tui.driver.binding_spec_count);
}

static void expansion_tui_driver_detail_line(size_t index,
                                             char *line,
                                             size_t line_len)
{
    switch (index) {
    case 0U:
        snprintf(line,
                 line_len,
                 "summary: %s",
                 expansion_tui.driver.summary != NULL ?
                     expansion_tui.driver.summary : "-");
        break;
    case 1U:
        snprintf(line,
                 line_len,
                 "category: %s",
                 solar_os_expansion_category_name(expansion_tui.driver.category));
        break;
    case 2U:
        snprintf(line,
                 line_len,
                 "support: %s",
                 solar_os_expansion_driver_supported(expansion_tui.driver.name) ?
                     "available" : "unsupported on this board");
        break;
    case 3U:
        snprintf(line,
                 line_len,
                 "probe: %s",
                 expansion_tui.driver.probe_supported ? "yes" : "no");
        break;
    case 4U:
        strlcpy(line, "bindings:", line_len);
        break;
    default:
        if (expansion_tui.driver.allow_unlisted_bindings) {
            strlcpy(line, "  one or more resource assignments", line_len);
        } else {
            const solar_os_expansion_binding_spec_t *spec =
                &expansion_tui.driver.binding_specs[index - 5U];
            snprintf(line,
                     line_len,
                     "  %c %-15s <%s>",
                     spec->required ? '*' : '-',
                     spec->key,
                     spec->value_hint != NULL ? spec->value_hint : "value");
        }
        break;
    }
}

static void expansion_tui_render_driver_detail(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = expansion_tui_driver_detail_count();
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, "driver", expansion_tui.driver.name);
    expansion_tui_draw_tabs(false);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.detail, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.detail.top + row;
        if (index >= count) {
            break;
        }
        char line[EXPANSION_TUI_MESSAGE_MAX];
        expansion_tui_driver_detail_line(index, line, sizeof(line));
        solar_os_tui_write_cell(tui, 2U + row, 0U, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    expansion_tui_draw_help(solar_os_expansion_driver_supported(expansion_tui.driver.name) ?
                                "arrows scroll  A/enter attach  esc back" :
                                "arrows scroll  unsupported  esc back");
    expansion_tui_finish_render(false);
}

static bool expansion_tui_device_exists(const char *name)
{
    for (size_t i = 0U; i < solar_os_expansion_device_count(); i++) {
        solar_os_expansion_device_t device;
        if (solar_os_expansion_get_device(i, &device) &&
            strcmp(device.name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void expansion_tui_default_device_name(void)
{
    for (unsigned suffix = 0U; suffix < 100U; suffix++) {
        snprintf(expansion_tui.device_name,
                 sizeof(expansion_tui.device_name),
                 "%.16s%u",
                 expansion_tui.driver.name,
                 suffix);
        if (!expansion_tui_device_exists(expansion_tui.device_name)) {
            return;
        }
    }
    expansion_tui.device_name[0] = '\0';
}

static bool expansion_tui_first_bus_value(solar_os_expansion_binding_kind_t kind,
                                          char *value,
                                          size_t value_len)
{
    if (kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS &&
        solar_os_expansion_i2c_bus_count() > 0U) {
        solar_os_expansion_i2c_bus_t bus;
        if (solar_os_expansion_get_i2c_bus(0U, &bus)) {
            strlcpy(value, bus.name, value_len);
            return true;
        }
    } else if (kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS &&
               solar_os_expansion_spi_bus_count() > 0U) {
        solar_os_expansion_spi_bus_t bus;
        if (solar_os_expansion_get_spi_bus(0U, &bus)) {
            strlcpy(value, bus.name, value_len);
            return true;
        }
    } else if (kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT &&
               solar_os_expansion_uart_port_count() > 0U) {
        solar_os_expansion_uart_port_t port;
        if (solar_os_expansion_get_uart_port(0U, &port)) {
            strlcpy(value, port.name, value_len);
            return true;
        }
    } else if (kind == SOLAR_OS_EXPANSION_BINDING_PS2_BUS &&
               solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_PS2) > 0U) {
        solar_os_bus_info_t bus;
        if (solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_PS2, 0U, &bus)) {
            strlcpy(value, bus.name, value_len);
            return true;
        }
    } else if (kind == SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM) {
        for (size_t i = 0U; i < solar_os_stream_count(); i++) {
            solar_os_stream_info_t stream;
            if (solar_os_stream_get(i, &stream) &&
                stream.type == SOLAR_OS_STREAM_TYPE_SCALAR &&
                stream.direction != SOLAR_OS_STREAM_DIRECTION_SINK) {
                strlcpy(value, stream.id, value_len);
                return true;
            }
        }
    }
    return false;
}

static void expansion_tui_prefill_form(void)
{
    memset(expansion_tui.values, 0, sizeof(expansion_tui.values));
    memset(expansion_tui.manual_resources, 0, sizeof(expansion_tui.manual_resources));
    expansion_tui_default_device_name();

    const size_t count = expansion_tui.driver.binding_spec_count <
            EXPANSION_TUI_FORM_SPEC_MAX ?
        expansion_tui.driver.binding_spec_count : EXPANSION_TUI_FORM_SPEC_MAX;
    for (size_t i = 0U; i < count; i++) {
        const solar_os_expansion_binding_spec_t *spec =
            &expansion_tui.driver.binding_specs[i];
        if (!spec->required) {
            continue;
        }
        if (spec->allowed_value_count > 0U) {
            if (spec->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS) {
                snprintf(expansion_tui.values[i],
                         sizeof(expansion_tui.values[i]),
                         "0x%02x",
                         spec->allowed_values[0]);
            } else if (spec->kind == SOLAR_OS_EXPANSION_BINDING_GPIO ||
                       spec->kind == SOLAR_OS_EXPANSION_BINDING_ADC ||
                       spec->kind == SOLAR_OS_EXPANSION_BINDING_PWM ||
                       spec->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) {
                snprintf(expansion_tui.values[i],
                         sizeof(expansion_tui.values[i]),
                         "gpio%d",
                         spec->allowed_values[0]);
            } else {
                snprintf(expansion_tui.values[i],
                         sizeof(expansion_tui.values[i]),
                         "%d",
                         spec->allowed_values[0]);
            }
        } else if (!expansion_tui_first_bus_value(spec->kind,
                                                  expansion_tui.values[i],
                                                  sizeof(expansion_tui.values[i])) &&
                   spec->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) {
            strlcpy(expansion_tui.values[i], "i2s0", sizeof(expansion_tui.values[i]));
        }
    }
}

static size_t expansion_tui_form_input_count(void)
{
    return expansion_tui.driver.allow_unlisted_bindings ?
        2U : 1U + expansion_tui.driver.binding_spec_count;
}

static char *expansion_tui_form_value(size_t field, size_t *capacity)
{
    if (field == 0U) {
        *capacity = sizeof(expansion_tui.device_name);
        return expansion_tui.device_name;
    }
    if (expansion_tui.driver.allow_unlisted_bindings) {
        *capacity = sizeof(expansion_tui.manual_resources);
        return expansion_tui.manual_resources;
    }
    if (field - 1U >= EXPANSION_TUI_FORM_SPEC_MAX) {
        return NULL;
    }
    *capacity = sizeof(expansion_tui.values[field - 1U]);
    return expansion_tui.values[field - 1U];
}

static void expansion_tui_form_label(size_t field, char *label, size_t label_len)
{
    if (field == 0U) {
        strlcpy(label, "* name: ", label_len);
    } else if (expansion_tui.driver.allow_unlisted_bindings) {
        strlcpy(label, "* resources: ", label_len);
    } else {
        const solar_os_expansion_binding_spec_t *spec =
            &expansion_tui.driver.binding_specs[field - 1U];
        snprintf(label,
                 label_len,
                 "%c %s <%s>: ",
                 spec->required ? '*' : '-',
                 spec->key,
                 spec->value_hint != NULL ? spec->value_hint : "value");
    }
}

static void expansion_tui_render_attach(void)
{
    solar_os_tui_t *tui = &expansion_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t input_count = expansion_tui_form_input_count();
    const size_t count = input_count + 1U;
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, "attach device", expansion_tui.driver.name);
    expansion_tui_draw_tabs(false);

    const size_t visible = expansion_tui_body_rows();
    solar_os_tui_viewport_reconcile(&expansion_tui.form, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = expansion_tui.form.top + row;
        if (index >= count) {
            break;
        }
        if (index == input_count) {
            solar_os_tui_write_cell(tui,
                                    2U + row,
                                    0U,
                                    cols,
                                    "[ Attach ]",
                                    index == expansion_tui.form.cursor ?
                                        SOLAR_OS_TUI_ATTR_BOLD |
                                            SOLAR_OS_TUI_ATTR_INVERSE :
                                        SOLAR_OS_TUI_ATTR_BOLD);
            continue;
        }

        char label[48];
        expansion_tui_form_label(index, label, sizeof(label));
        size_t capacity = 0U;
        char *value = expansion_tui_form_value(index, &capacity);
        if (value == NULL) {
            continue;
        }
        if (expansion_tui.editing && index == expansion_tui.form.cursor) {
            solar_os_tui_draw_input(tui,
                                    2U + row,
                                    0U,
                                    cols,
                                    label,
                                    value,
                                    &expansion_tui.input,
                                    SOLAR_OS_TUI_ATTR_INVERSE);
        } else {
            char line[EXPANSION_TUI_MESSAGE_MAX];
            snprintf(line, sizeof(line), "%s%s", label, value);
            solar_os_tui_write_cell(tui,
                                    2U + row,
                                    0U,
                                    cols,
                                    line,
                                    index == expansion_tui.form.cursor ?
                                        SOLAR_OS_TUI_ATTR_INVERSE :
                                        SOLAR_OS_TUI_ATTR_NORMAL);
        }
    }
    expansion_tui_draw_help(expansion_tui.editing ?
                                "type value  enter accepts  esc cancels edit" :
                                "arrows select  enter edit/attach  esc back");
    expansion_tui_finish_render(expansion_tui.editing);
}

static void expansion_tui_render(void)
{
    if (solar_os_tui_rows(&expansion_tui.tui) < 4U ||
        solar_os_tui_cols(&expansion_tui.tui) < 20U) {
        solar_os_tui_draw_too_small(&expansion_tui.tui, "expansion");
        expansion_tui_finish_render(false);
        return;
    }
    switch (expansion_tui.view) {
    case EXPANSION_TUI_VIEW_DEVICES:
        expansion_tui_render_devices();
        break;
    case EXPANSION_TUI_VIEW_DEVICE_DETAIL:
        expansion_tui_render_device_detail();
        break;
    case EXPANSION_TUI_VIEW_CATEGORIES:
        expansion_tui_render_categories();
        break;
    case EXPANSION_TUI_VIEW_DRIVERS:
        expansion_tui_render_drivers();
        break;
    case EXPANSION_TUI_VIEW_DRIVER_DETAIL:
        expansion_tui_render_driver_detail();
        break;
    case EXPANSION_TUI_VIEW_ATTACH:
        expansion_tui_render_attach();
        break;
    case EXPANSION_TUI_VIEW_DETACH_CONFIRM:
        expansion_tui_render_detach_confirm();
        break;
    default:
        break;
    }
}

static bool expansion_tui_parse_int(const char *text, int min, int max, int *value)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    if (strncmp(text, "gpio", 4U) == 0) {
        text += 4U;
    }
    if (strncmp(text, "i2s", 3U) == 0) {
        text += 3U;
    }
    errno = 0;
    char *end = NULL;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (int)parsed;
    return true;
}

static bool expansion_tui_binding_from_spec(
    const solar_os_expansion_binding_spec_t *spec,
    const char *text,
    solar_os_expansion_binding_t *binding,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    *binding = (solar_os_expansion_binding_t) {
        .kind = spec->kind,
        .value = -1,
        .aux = -1,
    };
    strlcpy(binding->role,
            spec->role != NULL ? spec->role : spec->key,
            sizeof(binding->role));

    switch (spec->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        if (!solar_os_expansion_find_i2c_bus(text, NULL, NULL)) return false;
        strlcpy(binding->target, text, sizeof(binding->target));
        return true;
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        if (!solar_os_expansion_find_spi_bus(text, NULL, NULL)) return false;
        strlcpy(binding->target, text, sizeof(binding->target));
        return true;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT: {
        solar_os_expansion_uart_port_t port;
        if (!solar_os_expansion_find_uart_port(text, &port, NULL)) return false;
        strlcpy(binding->target, text, sizeof(binding->target));
        binding->value = port.port;
        return true;
    }
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        if (!solar_os_bus_find(text, SOLAR_OS_BUS_PROTOCOL_PS2, NULL)) return false;
        strlcpy(binding->target, text, sizeof(binding->target));
        return true;
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM: {
        solar_os_stream_info_t stream;
        if (solar_os_stream_get_info(text, &stream) != ESP_OK ||
            stream.type != SOLAR_OS_STREAM_TYPE_SCALAR ||
            stream.direction == SOLAR_OS_STREAM_DIRECTION_SINK) {
            return false;
        }
        strlcpy(binding->target, text, sizeof(binding->target));
        return true;
    }
    case SOLAR_OS_EXPANSION_BINDING_I2S_PORT:
        return expansion_tui_parse_int(text, 0, 1, &binding->value);
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        return expansion_tui_parse_int(text, 0x03, 0x77, &binding->value);
    case SOLAR_OS_EXPANSION_BINDING_GPIO_LINE: {
        solar_os_gpio_line_ref_t line;
        if (!solar_os_gpio_line_parse(text, &line) ||
            (!solar_os_gpio_line_is_native(&line) &&
             !solar_os_gpio_controller_find(line.controller, NULL))) {
            return false;
        }
        strlcpy(binding->target, line.controller, sizeof(binding->target));
        binding->value = line.line;
        return true;
    }
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return expansion_tui_parse_int(text, 0, 63, &binding->value);
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        if (!expansion_tui_parse_int(text, 0, 63, &binding->value)) return false;
        for (size_t i = 0U; i < binding_count; i++) {
            if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
                strlcpy(binding->target,
                        bindings[i].target,
                        sizeof(binding->target));
                break;
            }
        }
        return binding->target[0] != '\0';
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return expansion_tui_parse_int(text, INT_MIN, INT_MAX, &binding->value);
    default:
        return false;
    }
}

static bool expansion_tui_build_bindings(solar_os_expansion_binding_t *bindings,
                                         size_t *binding_count,
                                         char *invalid_key,
                                         size_t invalid_key_len)
{
    *binding_count = 0U;
    invalid_key[0] = '\0';
    if (expansion_tui.driver.allow_unlisted_bindings) {
        char resources[EXPANSION_TUI_MANUAL_MAX];
        strlcpy(resources, expansion_tui.manual_resources, sizeof(resources));
        char *save = NULL;
        for (char *token = strtok_r(resources, " \t", &save);
             token != NULL;
             token = strtok_r(NULL, " \t", &save)) {
            if (!solar_os_shell_expansion_parse_binding_token(token,
                                                               bindings,
                                                               binding_count)) {
                strlcpy(invalid_key, token, invalid_key_len);
                return false;
            }
        }
        return true;
    }

    if (expansion_tui.driver.binding_spec_count > EXPANSION_TUI_FORM_SPEC_MAX) {
        strlcpy(invalid_key, "too many bindings", invalid_key_len);
        return false;
    }
    for (size_t i = 0U; i < expansion_tui.driver.binding_spec_count; i++) {
        const char *value = expansion_tui.values[i];
        if (value[0] == '\0') {
            continue;
        }
        if (*binding_count >= SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
            !expansion_tui_binding_from_spec(&expansion_tui.driver.binding_specs[i],
                                             value,
                                             &bindings[*binding_count],
                                             bindings,
                                             *binding_count)) {
            strlcpy(invalid_key,
                    expansion_tui.driver.binding_specs[i].key,
                    invalid_key_len);
            return false;
        }
        (*binding_count)++;
    }
    return true;
}

static void expansion_tui_validation_message(
    const solar_os_expansion_binding_validation_t *validation)
{
    const char *reason = "invalid";
    switch (validation->reason) {
    case SOLAR_OS_EXPANSION_BINDINGS_MISSING:
        reason = "missing";
        break;
    case SOLAR_OS_EXPANSION_BINDINGS_UNEXPECTED:
        reason = "unexpected";
        break;
    case SOLAR_OS_EXPANSION_BINDINGS_DUPLICATE:
        reason = "duplicate";
        break;
    case SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE:
        reason = "invalid value";
        break;
    case SOLAR_OS_EXPANSION_BINDINGS_UNAVAILABLE:
        reason = "unavailable";
        break;
    default:
        break;
    }
    snprintf(expansion_tui.message,
             sizeof(expansion_tui.message),
             "%s binding: %s",
             reason,
             validation->key[0] != '\0' ? validation->key : "resources");
}

static void expansion_tui_attach(void)
{
    if (expansion_tui.device_name[0] == '\0') {
        expansion_tui_set_message("device name is required");
        return;
    }

    solar_os_expansion_binding_t bindings[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    size_t binding_count = 0U;
    char invalid_key[32];
    if (!expansion_tui_build_bindings(bindings,
                                      &binding_count,
                                      invalid_key,
                                      sizeof(invalid_key))) {
        snprintf(expansion_tui.message,
                 sizeof(expansion_tui.message),
                 "invalid binding: %s",
                 invalid_key);
        return;
    }

    solar_os_expansion_binding_validation_t validation;
    const esp_err_t validation_err = solar_os_expansion_validate_bindings(
        expansion_tui.driver.name,
        bindings,
        binding_count,
        &validation);
    if (validation_err != ESP_OK) {
        expansion_tui_validation_message(&validation);
        return;
    }

    const esp_err_t err = solar_os_expansion_attach(expansion_tui.driver.name,
                                                     expansion_tui.device_name,
                                                     bindings,
                                                     binding_count);
    if (err != ESP_OK) {
        expansion_tui_set_error("attach", err);
        return;
    }
    snprintf(expansion_tui.message,
             sizeof(expansion_tui.message),
             "attached %s",
             expansion_tui.device_name);
    expansion_tui.view = EXPANSION_TUI_VIEW_DEVICES;
    const size_t count = solar_os_expansion_device_count();
    expansion_tui.devices.cursor = count > 0U ? count - 1U : 0U;
    expansion_tui.devices.top = 0U;
    expansion_tui.editing = false;
}

static void expansion_tui_open_attach(void)
{
    if (!solar_os_expansion_driver_supported(expansion_tui.driver.name)) {
        expansion_tui_set_message("driver is unsupported on this board");
        return;
    }
    if (expansion_tui.driver.binding_spec_count > EXPANSION_TUI_FORM_SPEC_MAX) {
        expansion_tui_set_message("driver has too many bindings for the TUI");
        return;
    }
    expansion_tui_prefill_form();
    expansion_tui.form = (solar_os_tui_viewport_t) {0};
    expansion_tui.input = (solar_os_tui_input_state_t) {0};
    expansion_tui.editing = false;
    expansion_tui.view = EXPANSION_TUI_VIEW_ATTACH;
    expansion_tui_set_message("");
}

static void expansion_tui_switch_root(bool devices)
{
    expansion_tui.view = devices ?
        EXPANSION_TUI_VIEW_DEVICES : EXPANSION_TUI_VIEW_CATEGORIES;
    expansion_tui_set_message("");
}

static void expansion_tui_handle_devices(uint8_t key)
{
    const size_t count = solar_os_expansion_device_count();
    if (key == '\t' || key == SOLAR_OS_KEY_RIGHT) {
        expansion_tui_switch_root(false);
    } else if (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q') {
        solar_os_context_finish(expansion_tui.ctx, 0, NULL);
        return;
    } else if (key == 'n' || key == 'N') {
        expansion_tui_switch_root(false);
    } else if ((key == SOLAR_OS_KEY_ENTER || key == '\r') && count > 0U &&
               solar_os_expansion_get_device(expansion_tui.devices.cursor,
                                             &expansion_tui.device)) {
        expansion_tui.detail = (solar_os_tui_viewport_t) {0};
        expansion_tui.view = EXPANSION_TUI_VIEW_DEVICE_DETAIL;
        expansion_tui_set_message("");
    } else if (solar_os_tui_viewport_key(&expansion_tui.devices,
                                         key,
                                         count,
                                         expansion_tui_body_rows(),
                                         false)) {
        expansion_tui_set_message("");
    }
    expansion_tui_render();
}

static void expansion_tui_handle_device_detail(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE) {
        expansion_tui.view = EXPANSION_TUI_VIEW_DEVICES;
        expansion_tui_set_message("");
    } else if (key == 'd' || key == 'D' || key == SOLAR_OS_KEY_DELETE) {
        if (expansion_tui.device.detachable) {
            expansion_tui.view = EXPANSION_TUI_VIEW_DETACH_CONFIRM;
        } else {
            expansion_tui_set_message("fixed devices cannot be detached");
        }
    } else {
        solar_os_tui_viewport_key(&expansion_tui.detail,
                                  key,
                                  expansion_tui_device_detail_count(),
                                  expansion_tui_body_rows(),
                                  false);
    }
    expansion_tui_render();
}

static void expansion_tui_handle_detach_confirm(uint8_t key)
{
    if (key == 'y' || key == 'Y') {
        const esp_err_t err = solar_os_expansion_detach(expansion_tui.device.name);
        if (err == ESP_OK) {
            snprintf(expansion_tui.message,
                     sizeof(expansion_tui.message),
                     "detached %s",
                     expansion_tui.device.name);
            expansion_tui.view = EXPANSION_TUI_VIEW_DEVICES;
            const size_t count = solar_os_expansion_device_count();
            if (expansion_tui.devices.cursor >= count && count > 0U) {
                expansion_tui.devices.cursor = count - 1U;
            }
        } else {
            expansion_tui_set_error("detach", err);
            expansion_tui.view = EXPANSION_TUI_VIEW_DEVICE_DETAIL;
        }
    } else {
        expansion_tui.view = EXPANSION_TUI_VIEW_DEVICE_DETAIL;
        expansion_tui_set_message("detach cancelled");
    }
    expansion_tui_render();
}

static void expansion_tui_handle_categories(uint8_t key)
{
    const size_t count = expansion_tui_category_count();
    if (key == '\t' || key == SOLAR_OS_KEY_LEFT) {
        expansion_tui_switch_root(true);
    } else if (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q') {
        solar_os_context_finish(expansion_tui.ctx, 0, NULL);
        return;
    } else if ((key == SOLAR_OS_KEY_ENTER || key == '\r') &&
               expansion_tui_get_category(expansion_tui.categories.cursor,
                                          &expansion_tui.category)) {
        expansion_tui.drivers = (solar_os_tui_viewport_t) {0};
        expansion_tui.view = EXPANSION_TUI_VIEW_DRIVERS;
        expansion_tui_set_message("");
    } else if (solar_os_tui_viewport_key(&expansion_tui.categories,
                                         key,
                                         count,
                                         expansion_tui_body_rows(),
                                         false)) {
        expansion_tui_set_message("");
    }
    expansion_tui_render();
}

static void expansion_tui_handle_drivers(uint8_t key)
{
    const size_t count = expansion_tui_category_driver_count(expansion_tui.category);
    if (key == SOLAR_OS_KEY_ESCAPE) {
        expansion_tui.view = EXPANSION_TUI_VIEW_CATEGORIES;
        expansion_tui_set_message("");
    } else if ((key == SOLAR_OS_KEY_ENTER || key == '\r') &&
               expansion_tui_get_driver(expansion_tui.category,
                                        expansion_tui.drivers.cursor,
                                        &expansion_tui.driver)) {
        expansion_tui.detail = (solar_os_tui_viewport_t) {0};
        expansion_tui.view = EXPANSION_TUI_VIEW_DRIVER_DETAIL;
        expansion_tui_set_message("");
    } else if (solar_os_tui_viewport_key(&expansion_tui.drivers,
                                         key,
                                         count,
                                         expansion_tui_body_rows(),
                                         false)) {
        expansion_tui_set_message("");
    }
    expansion_tui_render();
}

static void expansion_tui_handle_driver_detail(uint8_t key)
{
    if (key == SOLAR_OS_KEY_ESCAPE) {
        expansion_tui.view = EXPANSION_TUI_VIEW_DRIVERS;
        expansion_tui_set_message("");
    } else if (key == SOLAR_OS_KEY_ENTER || key == '\r' || key == 'a' || key == 'A') {
        expansion_tui_open_attach();
    } else {
        solar_os_tui_viewport_key(&expansion_tui.detail,
                                  key,
                                  expansion_tui_driver_detail_count(),
                                  expansion_tui_body_rows(),
                                  false);
    }
    expansion_tui_render();
}

static void expansion_tui_handle_attach(uint8_t key)
{
    const size_t input_count = expansion_tui_form_input_count();
    if (expansion_tui.editing) {
        size_t capacity = 0U;
        char *value = expansion_tui_form_value(expansion_tui.form.cursor, &capacity);
        char label[48];
        expansion_tui_form_label(expansion_tui.form.cursor, label, sizeof(label));
        const size_t cols = solar_os_tui_cols(&expansion_tui.tui);
        const size_t label_width = strlen(label);
        const solar_os_tui_input_action_t action = solar_os_tui_input_key(
            value,
            capacity,
            &expansion_tui.input,
            key,
            cols > label_width ? cols - label_width : 1U);
        if (action == SOLAR_OS_TUI_INPUT_CANCEL ||
            action == SOLAR_OS_TUI_INPUT_SUBMIT) {
            expansion_tui.editing = false;
            expansion_tui.input = (solar_os_tui_input_state_t) {0};
        } else if (action == SOLAR_OS_TUI_INPUT_CHANGED) {
            expansion_tui_set_message("");
        }
    } else if (key == SOLAR_OS_KEY_ESCAPE) {
        expansion_tui.view = EXPANSION_TUI_VIEW_DRIVER_DETAIL;
        expansion_tui_set_message("");
    } else if (key == SOLAR_OS_KEY_ENTER || key == '\r') {
        if (expansion_tui.form.cursor == input_count) {
            expansion_tui_attach();
        } else {
            size_t capacity = 0U;
            char *value = expansion_tui_form_value(expansion_tui.form.cursor, &capacity);
            (void)capacity;
            expansion_tui.input.cursor = value != NULL ? strlen(value) : 0U;
            expansion_tui.input.view = 0U;
            expansion_tui.editing = value != NULL;
            expansion_tui_set_message("");
        }
    } else if (solar_os_tui_viewport_key(&expansion_tui.form,
                                         key,
                                         input_count + 1U,
                                         expansion_tui_body_rows(),
                                         false)) {
        expansion_tui_set_message("");
    }
    expansion_tui_render();
}

static esp_err_t expansion_tui_start(solar_os_context_t *ctx)
{
    memset(&expansion_tui, 0, sizeof(expansion_tui));
    expansion_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_screen_begin(&expansion_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    solar_os_tui_set_cursor_visible(&expansion_tui.tui, false);
    expansion_tui_render();
    return ESP_OK;
}

static void expansion_tui_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&expansion_tui.tui, true);
    solar_os_tui_refresh(&expansion_tui.tui);
}

static void expansion_tui_resume(solar_os_context_t *ctx)
{
    expansion_tui.ctx = ctx;
    expansion_tui_render();
}

static void expansion_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&expansion_tui.tui, true);
    solar_os_tui_clear(&expansion_tui.tui);
    solar_os_tui_refresh(&expansion_tui.tui);
    solar_os_tui_end(&expansion_tui.tui);
    memset(&expansion_tui, 0, sizeof(expansion_tui));
}

static bool expansion_tui_event(solar_os_context_t *ctx,
                                const solar_os_event_t *event)
{
    (void)ctx;
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        expansion_tui_render();
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }
    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(expansion_tui.ctx, 0, NULL);
        return true;
    }
    switch (expansion_tui.view) {
    case EXPANSION_TUI_VIEW_DEVICES:
        expansion_tui_handle_devices(key);
        break;
    case EXPANSION_TUI_VIEW_DEVICE_DETAIL:
        expansion_tui_handle_device_detail(key);
        break;
    case EXPANSION_TUI_VIEW_DETACH_CONFIRM:
        expansion_tui_handle_detach_confirm(key);
        break;
    case EXPANSION_TUI_VIEW_CATEGORIES:
        expansion_tui_handle_categories(key);
        break;
    case EXPANSION_TUI_VIEW_DRIVERS:
        expansion_tui_handle_drivers(key);
        break;
    case EXPANSION_TUI_VIEW_DRIVER_DETAIL:
        expansion_tui_handle_driver_detail(key);
        break;
    case EXPANSION_TUI_VIEW_ATTACH:
        expansion_tui_handle_attach(key);
        break;
    default:
        break;
    }
    return true;
}

static void expansion_tui_title(solar_os_context_t *ctx,
                                char *buffer,
                                size_t buffer_len)
{
    (void)ctx;
    snprintf(buffer, buffer_len, "Expansion");
}

static const solar_os_app_t expansion_tui_app = {
    .name = "expansion",
    .summary = "expansion device manager",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = expansion_tui_start,
    .suspend = expansion_tui_suspend,
    .resume = expansion_tui_resume,
    .stop = expansion_tui_stop,
    .event = expansion_tui_event,
    .title = expansion_tui_title,
    .state_slot = &expansion_tui_state,
    .state_size = sizeof(expansion_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
};

esp_err_t solar_os_shell_launch_expansion_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &expansion_tui_app, 0, NULL);
}
