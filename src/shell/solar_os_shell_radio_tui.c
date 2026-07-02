#include "solar_os_shell_tui_apps.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "solar_os_keys.h"
#include "solar_os_radio.h"
#include "solar_os_tui.h"

#define RADIO_TUI_STATUS_MAX 96
#define RADIO_TUI_EDIT_MAX 64
#define RADIO_TUI_REFRESH_MS 250

typedef enum {
    RADIO_TUI_ITEM_DEVICE,
    RADIO_TUI_ITEM_STATE,
    RADIO_TUI_ITEM_RSSI,
    RADIO_TUI_ITEM_FREQ,
    RADIO_TUI_ITEM_MODULATION,
    RADIO_TUI_ITEM_BITRATE,
    RADIO_TUI_ITEM_DEVIATION,
    RADIO_TUI_ITEM_BANDWIDTH,
    RADIO_TUI_ITEM_POWER,
    RADIO_TUI_ITEM_CRC,
    RADIO_TUI_ITEM_VARIABLE,
    RADIO_TUI_ITEM_PREAMBLE,
    RADIO_TUI_ITEM_SYNC,
    RADIO_TUI_ITEM_COUNT,
} radio_tui_item_t;

typedef struct {
    const char *label;
    bool editable;
} radio_tui_item_def_t;

typedef struct {
    solar_os_context_t *ctx;
    solar_os_tui_t tui;
    size_t selected;
    size_t first_item;
    size_t radio_index;
    char status[RADIO_TUI_STATUS_MAX];
    char edit_text[RADIO_TUI_EDIT_MAX];
    char original_text[RADIO_TUI_EDIT_MAX];
    bool editing;
    bool cursor_visible;
    uint32_t last_refresh_ms;
    uint32_t last_cursor_blink_ms;
} radio_tui_state_t;

static radio_tui_state_t radio_tui;

static const radio_tui_item_def_t radio_tui_items[] = {
    [RADIO_TUI_ITEM_DEVICE] = {.label = "device", .editable = false},
    [RADIO_TUI_ITEM_STATE] = {.label = "state", .editable = false},
    [RADIO_TUI_ITEM_RSSI] = {.label = "rssi", .editable = false},
    [RADIO_TUI_ITEM_FREQ] = {.label = "frequency", .editable = true},
    [RADIO_TUI_ITEM_MODULATION] = {.label = "modulation", .editable = false},
    [RADIO_TUI_ITEM_BITRATE] = {.label = "bitrate", .editable = true},
    [RADIO_TUI_ITEM_DEVIATION] = {.label = "deviation", .editable = true},
    [RADIO_TUI_ITEM_BANDWIDTH] = {.label = "bandwidth", .editable = true},
    [RADIO_TUI_ITEM_POWER] = {.label = "power", .editable = true},
    [RADIO_TUI_ITEM_CRC] = {.label = "crc", .editable = false},
    [RADIO_TUI_ITEM_VARIABLE] = {.label = "variable", .editable = false},
    [RADIO_TUI_ITEM_PREAMBLE] = {.label = "preamble", .editable = true},
    [RADIO_TUI_ITEM_SYNC] = {.label = "sync", .editable = true},
};

static void radio_tui_set_status(const char *status)
{
    strlcpy(radio_tui.status, status != NULL ? status : "", sizeof(radio_tui.status));
}

static size_t radio_tui_visible_width(size_t cols, size_t start_col)
{
    return start_col < cols ? cols - start_col : 0;
}

static void radio_tui_write_cell(size_t row,
                                 size_t col,
                                 size_t width,
                                 const char *text,
                                 uint8_t attr)
{
    char clipped[RADIO_TUI_STATUS_MAX];
    size_t len = 0;

    if (width == 0) {
        return;
    }

    solar_os_tui_fill(&radio_tui.tui, row, col, 1, width, ' ', attr);
    if (text == NULL || text[0] == '\0') {
        return;
    }

    while (text[len] != '\0' && len + 1 < sizeof(clipped) && len < width) {
        clipped[len] = text[len];
        len++;
    }
    clipped[len] = '\0';
    solar_os_tui_addstr(&radio_tui.tui, row, col, clipped, attr);
}

static bool radio_tui_current(solar_os_radio_info_t *info, solar_os_radio_status_t *status)
{
    const size_t count = solar_os_radio_count();
    if (count == 0) {
        return false;
    }
    if (radio_tui.radio_index >= count) {
        radio_tui.radio_index = count - 1U;
    }
    if (!solar_os_radio_get(radio_tui.radio_index, info)) {
        return false;
    }
    if (status != NULL && solar_os_radio_get_status(info->name, status) != ESP_OK) {
        memset(status, 0, sizeof(*status));
        status->state = SOLAR_OS_RADIO_STATE_UNKNOWN;
    }
    return true;
}

static bool radio_tui_parse_u32(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

static bool radio_tui_parse_i32(const char *text, int32_t min, int32_t max, int32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed < min || parsed > max) {
        return false;
    }
    *value = (int32_t)parsed;
    return true;
}

static bool radio_tui_token_equals_ci(const char *start, const char *end, const char *token)
{
    const char *p = start;
    const char *q = token;

    while (p < end && *q != '\0') {
        if (tolower((unsigned char)*p) != tolower((unsigned char)*q)) {
            return false;
        }
        p++;
        q++;
    }
    return p == end && *q == '\0';
}

static bool radio_tui_parse_frequency(const char *text, uint32_t min, uint32_t max, uint32_t *value)
{
    if (text == NULL || text[0] == '\0' || value == NULL) {
        return false;
    }

    const char *start = text;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    const char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (start == end) {
        return false;
    }

    const char *p = start;
    uint64_t whole = 0;
    bool has_digit = false;
    while (p < end && isdigit((unsigned char)*p)) {
        const uint64_t digit = (uint64_t)(*p - '0');
        if (whole > (UINT64_MAX - digit) / 10ULL) {
            return false;
        }
        whole = whole * 10ULL + digit;
        has_digit = true;
        p++;
    }
    if (!has_digit) {
        return false;
    }

    uint64_t fraction = 0;
    uint64_t fraction_scale = 1;
    bool has_fraction = false;
    if (p < end && *p == '.') {
        p++;
        while (p < end && isdigit((unsigned char)*p)) {
            if (fraction_scale > 100000000ULL) {
                return false;
            }
            fraction = fraction * 10ULL + (uint64_t)(*p - '0');
            fraction_scale *= 10ULL;
            has_fraction = true;
            p++;
        }
        if (!has_fraction) {
            return false;
        }
    }

    while (p < end && isspace((unsigned char)*p)) {
        p++;
    }

    uint64_t multiplier = 1;
    if (p == end) {
        multiplier = 1;
    } else if (radio_tui_token_equals_ci(p, end, "hz")) {
        multiplier = 1;
    } else if (radio_tui_token_equals_ci(p, end, "k") ||
               radio_tui_token_equals_ci(p, end, "khz")) {
        multiplier = 1000ULL;
    } else if (radio_tui_token_equals_ci(p, end, "m") ||
               radio_tui_token_equals_ci(p, end, "mhz")) {
        multiplier = 1000000ULL;
    } else {
        return false;
    }

    if (has_fraction && multiplier == 1) {
        return false;
    }
    if (whole > UINT64_MAX / multiplier) {
        return false;
    }
    uint64_t hz = whole * multiplier;
    if (has_fraction) {
        if (fraction > UINT64_MAX / multiplier) {
            return false;
        }
        const uint64_t fractional_hz = (fraction * multiplier + fraction_scale / 2ULL) / fraction_scale;
        if (hz > UINT64_MAX - fractional_hz) {
            return false;
        }
        hz += fractional_hz;
    }
    if (hz < min || hz > max) {
        return false;
    }

    *value = (uint32_t)hz;
    return true;
}

static void radio_tui_format_sync(const solar_os_radio_config_t *config, char *buffer, size_t buffer_len)
{
    size_t used = 0;

    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';
    if (config == NULL || config->sync_word_len == 0) {
        strlcpy(buffer, "none", buffer_len);
        return;
    }

    for (uint8_t i = 0; i < config->sync_word_len && used + 1 < buffer_len; i++) {
        const int written = snprintf(&buffer[used],
                                     buffer_len - used,
                                     "%s%02x",
                                     i == 0 ? "" : ":",
                                     config->sync_word[i]);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= buffer_len - used) {
            used = buffer_len - 1U;
            break;
        }
        used += (size_t)written;
    }
}

static bool radio_tui_parse_sync(const char *text, solar_os_radio_config_t *config)
{
    char copy[RADIO_TUI_EDIT_MAX];
    char *save = NULL;
    uint8_t bytes[SOLAR_OS_RADIO_SYNC_WORD_MAX];
    uint8_t count = 0;

    if (text == NULL || config == NULL) {
        return false;
    }
    strlcpy(copy, text, sizeof(copy));

    char *start = copy;
    while (isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    if (strcmp(start, "none") == 0 || strcmp(start, "-") == 0) {
        config->sync_word_len = 0;
        memset(config->sync_word, 0, sizeof(config->sync_word));
        return true;
    }

    for (char *token = strtok_r(start, ":, ", &save);
         token != NULL;
         token = strtok_r(NULL, ":, ", &save)) {
        if (count >= SOLAR_OS_RADIO_SYNC_WORD_MAX) {
            return false;
        }
        char *token_end = NULL;
        errno = 0;
        const long parsed = strtol(token, &token_end, 16);
        if (errno != 0 || token_end == token || *token_end != '\0' || parsed < 0 || parsed > 255) {
            return false;
        }
        bytes[count++] = (uint8_t)parsed;
    }

    if (count == 0) {
        return false;
    }
    config->sync_word_len = count;
    memset(config->sync_word, 0, sizeof(config->sync_word));
    memcpy(config->sync_word, bytes, count);
    return true;
}

static void radio_tui_current_value(radio_tui_item_t item,
                                    const solar_os_radio_info_t *info,
                                    const solar_os_radio_status_t *status,
                                    char *buffer,
                                    size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    buffer[0] = '\0';

    if (info == NULL || status == NULL) {
        strlcpy(buffer, "-", buffer_len);
        return;
    }

    const solar_os_radio_config_t *config = &status->config;
    switch (item) {
    case RADIO_TUI_ITEM_DEVICE:
        snprintf(buffer, buffer_len, "%s", info->name);
        break;
    case RADIO_TUI_ITEM_STATE:
        snprintf(buffer, buffer_len, "%s", solar_os_radio_state_name(status->state));
        break;
    case RADIO_TUI_ITEM_RSSI:
        if (status->has_rssi) {
            snprintf(buffer, buffer_len, "%d dBm", (int)status->rssi_dbm);
        } else {
            strlcpy(buffer, "-", buffer_len);
        }
        break;
    case RADIO_TUI_ITEM_FREQ:
        snprintf(buffer, buffer_len, "%" PRIu32, config->frequency_hz);
        break;
    case RADIO_TUI_ITEM_MODULATION:
        snprintf(buffer, buffer_len, "%s", solar_os_radio_modulation_name(config->modulation));
        break;
    case RADIO_TUI_ITEM_BITRATE:
        snprintf(buffer, buffer_len, "%" PRIu32, config->bitrate_bps);
        break;
    case RADIO_TUI_ITEM_DEVIATION:
        snprintf(buffer, buffer_len, "%" PRIu32, config->deviation_hz);
        break;
    case RADIO_TUI_ITEM_BANDWIDTH:
        snprintf(buffer, buffer_len, "%" PRIu32, config->rx_bandwidth_hz);
        break;
    case RADIO_TUI_ITEM_POWER:
        snprintf(buffer, buffer_len, "%d", (int)config->tx_power_dbm);
        break;
    case RADIO_TUI_ITEM_CRC:
        strlcpy(buffer, config->crc_enabled ? "on" : "off", buffer_len);
        break;
    case RADIO_TUI_ITEM_VARIABLE:
        strlcpy(buffer, config->variable_length ? "on" : "off", buffer_len);
        break;
    case RADIO_TUI_ITEM_PREAMBLE:
        snprintf(buffer, buffer_len, "%u", (unsigned)config->preamble_len);
        break;
    case RADIO_TUI_ITEM_SYNC:
        radio_tui_format_sync(config, buffer, buffer_len);
        break;
    default:
        strlcpy(buffer, "-", buffer_len);
        break;
    }
}

static void radio_tui_update_scroll(size_t visible_items)
{
    if (visible_items == 0) {
        radio_tui.first_item = 0;
        return;
    }
    if (radio_tui.selected < radio_tui.first_item) {
        radio_tui.first_item = radio_tui.selected;
    }
    if (radio_tui.selected >= radio_tui.first_item + visible_items) {
        radio_tui.first_item = radio_tui.selected + 1U - visible_items;
    }
}

static void radio_tui_render(void)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    const bool has_radio = radio_tui_current(&info, &status);
    solar_os_tui_t *tui = &radio_tui.tui;
    const size_t rows = solar_os_tui_rows(tui);
    const size_t cols = solar_os_tui_cols(tui);

    if (rows == 0 || cols == 0) {
        return;
    }

    solar_os_tui_clear(tui);

    size_t split = cols / 2;
    if (cols >= 24 && split < 12) {
        split = 12;
    }
    if (split + 1 >= cols) {
        split = cols > 2 ? cols / 2 : 1;
    }

    radio_tui_write_cell(0,
                         0,
                         split,
                         "radio",
                         SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    if (cols > split) {
        solar_os_tui_vrule(tui, 0, split, rows, 1, SOLAR_OS_TUI_ATTR_NORMAL);
        radio_tui_write_cell(0,
                             split + 1,
                             radio_tui_visible_width(cols, split + 1),
                             "value",
                             SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE);
    }

    const size_t footer_row = rows - 1U;
    if (!has_radio) {
        if (rows > 1) {
            radio_tui_write_cell(1, 0, split, "device", SOLAR_OS_TUI_ATTR_NORMAL);
            radio_tui_write_cell(1,
                                 split + 1,
                                 radio_tui_visible_width(cols, split + 1),
                                 "none",
                                 SOLAR_OS_TUI_ATTR_NORMAL);
        }
        if (rows > 1) {
            radio_tui_write_cell(footer_row, 0, cols, "esc exits", SOLAR_OS_TUI_ATTR_INVERSE);
        }
        solar_os_tui_set_cursor_visible(tui, false);
        solar_os_tui_refresh(tui);
        return;
    }

    const size_t value_col = split + 1U;
    const size_t value_width = radio_tui_visible_width(cols, value_col);
    const size_t visible_items = rows > 2 ? rows - 2U : 0;
    radio_tui_update_scroll(visible_items);

    size_t edit_cursor_row = 0;
    bool edit_cursor_visible = false;

    for (size_t line = 0; line < visible_items; line++) {
        const size_t item_index = radio_tui.first_item + line;
        if (item_index >= RADIO_TUI_ITEM_COUNT) {
            break;
        }

        const bool selected = item_index == radio_tui.selected;
        uint8_t label_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        uint8_t value_attr = SOLAR_OS_TUI_ATTR_NORMAL;
        if (selected) {
            label_attr = SOLAR_OS_TUI_ATTR_BOLD | SOLAR_OS_TUI_ATTR_INVERSE;
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE;
        }
        if (radio_tui.editing && selected) {
            value_attr = SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_UNDERLINE;
        }
        char value[RADIO_TUI_STATUS_MAX];
        const size_t row = line + 1U;

        if (radio_tui.editing && selected) {
            strlcpy(value, radio_tui.edit_text, sizeof(value));
            edit_cursor_row = row;
            edit_cursor_visible = true;
        } else {
            radio_tui_current_value((radio_tui_item_t)item_index, &info, &status, value, sizeof(value));
        }

        radio_tui_write_cell(row, 0, split, radio_tui_items[item_index].label, label_attr);
        if (value_width > 0) {
            radio_tui_write_cell(row, value_col, value_width, value, value_attr);
        }
    }

    const char *footer = radio_tui.status[0] != '\0' ?
        radio_tui.status :
        (radio_tui.editing ? "enter saves, esc cancels" : "enter edits, arrows cycle, esc exits");
    if (rows > 1) {
        radio_tui_write_cell(footer_row, 0, cols, footer, SOLAR_OS_TUI_ATTR_INVERSE);
    }

    if (edit_cursor_visible && value_width > 0) {
        const size_t len = strlen(radio_tui.edit_text);
        const size_t cursor_col = value_col + (len < value_width ? len : value_width - 1U);
        solar_os_tui_move(tui, edit_cursor_row, cursor_col);
    }
    solar_os_tui_set_cursor_visible(tui, edit_cursor_visible && radio_tui.cursor_visible);
    solar_os_tui_refresh(tui);
}

static bool radio_tui_apply_config(const char *name, solar_os_radio_config_t *config)
{
    const esp_err_t err = solar_os_radio_configure(name, config);
    if (err == ESP_OK) {
        radio_tui_set_status("configured");
        return true;
    }

    char message[RADIO_TUI_STATUS_MAX];
    snprintf(message, sizeof(message), "configure failed: %s", esp_err_to_name(err));
    radio_tui_set_status(message);
    return false;
}

static bool radio_tui_commit_edit(void)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    uint32_t u32 = 0;
    int32_t i32 = 0;

    if (!radio_tui_current(&info, &status)) {
        radio_tui_set_status("no radio");
        return false;
    }

    solar_os_radio_config_t config = status.config;
    switch ((radio_tui_item_t)radio_tui.selected) {
    case RADIO_TUI_ITEM_FREQ:
        if (!radio_tui_parse_frequency(radio_tui.edit_text, 1, UINT32_MAX, &u32)) {
            radio_tui_set_status("invalid frequency");
            return false;
        }
        config.frequency_hz = u32;
        break;
    case RADIO_TUI_ITEM_BITRATE:
        if (!radio_tui_parse_u32(radio_tui.edit_text, 1, UINT32_MAX, &u32)) {
            radio_tui_set_status("invalid bitrate");
            return false;
        }
        config.bitrate_bps = u32;
        break;
    case RADIO_TUI_ITEM_DEVIATION:
        if (!radio_tui_parse_u32(radio_tui.edit_text, 0, UINT32_MAX, &u32)) {
            radio_tui_set_status("invalid deviation");
            return false;
        }
        config.deviation_hz = u32;
        break;
    case RADIO_TUI_ITEM_BANDWIDTH:
        if (!radio_tui_parse_u32(radio_tui.edit_text, 0, UINT32_MAX, &u32)) {
            radio_tui_set_status("invalid bandwidth");
            return false;
        }
        config.rx_bandwidth_hz = u32;
        break;
    case RADIO_TUI_ITEM_POWER:
        if (!radio_tui_parse_i32(radio_tui.edit_text, -128, 127, &i32)) {
            radio_tui_set_status("invalid power");
            return false;
        }
        config.tx_power_dbm = (int8_t)i32;
        break;
    case RADIO_TUI_ITEM_PREAMBLE:
        if (!radio_tui_parse_u32(radio_tui.edit_text, 0, UINT16_MAX, &u32)) {
            radio_tui_set_status("invalid preamble");
            return false;
        }
        config.preamble_len = (uint16_t)u32;
        break;
    case RADIO_TUI_ITEM_SYNC:
        if (!radio_tui_parse_sync(radio_tui.edit_text, &config)) {
            radio_tui_set_status("invalid sync");
            return false;
        }
        break;
    default:
        return false;
    }

    return radio_tui_apply_config(info.name, &config);
}

static void radio_tui_begin_edit(void)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;

    if (!radio_tui_items[radio_tui.selected].editable) {
        radio_tui_set_status("use arrows");
        radio_tui_render();
        return;
    }
    if (!radio_tui_current(&info, &status)) {
        radio_tui_set_status("no radio");
        radio_tui_render();
        return;
    }

    radio_tui_current_value((radio_tui_item_t)radio_tui.selected,
                            &info,
                            &status,
                            radio_tui.edit_text,
                            sizeof(radio_tui.edit_text));
    strlcpy(radio_tui.original_text, radio_tui.edit_text, sizeof(radio_tui.original_text));
    radio_tui.editing = true;
    radio_tui.cursor_visible = true;
    radio_tui.last_cursor_blink_ms = 0;
    radio_tui_set_status("");
    radio_tui_render();
}

static void radio_tui_cancel_edit(void)
{
    strlcpy(radio_tui.edit_text, radio_tui.original_text, sizeof(radio_tui.edit_text));
    radio_tui.editing = false;
    radio_tui.cursor_visible = false;
    radio_tui_set_status("");
    radio_tui_render();
}

static void radio_tui_finish_edit(void)
{
    if (radio_tui_commit_edit()) {
        radio_tui.editing = false;
        radio_tui.cursor_visible = false;
    } else {
        radio_tui.cursor_visible = true;
        radio_tui.last_cursor_blink_ms = 0;
    }
    radio_tui_render();
}

static solar_os_radio_state_t radio_tui_next_state(solar_os_radio_state_t current, int direction)
{
    static const solar_os_radio_state_t states[] = {
        SOLAR_OS_RADIO_STATE_SLEEP,
        SOLAR_OS_RADIO_STATE_STANDBY,
        SOLAR_OS_RADIO_STATE_RX,
    };
    size_t index = 1;

    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (states[i] == current) {
            index = i;
            break;
        }
    }

    const size_t count = sizeof(states) / sizeof(states[0]);
    if (direction < 0) {
        index = index == 0 ? count - 1U : index - 1U;
    } else {
        index = (index + 1U) % count;
    }
    return states[index];
}

static solar_os_radio_modulation_t radio_tui_next_modulation(solar_os_radio_modulation_t current,
                                                             solar_os_radio_modulations_t supported,
                                                             int direction)
{
    static const solar_os_radio_modulation_t values[] = {
        SOLAR_OS_RADIO_MODULATION_FSK,
        SOLAR_OS_RADIO_MODULATION_GFSK,
        SOLAR_OS_RADIO_MODULATION_MSK,
        SOLAR_OS_RADIO_MODULATION_GMSK,
        SOLAR_OS_RADIO_MODULATION_OOK,
        SOLAR_OS_RADIO_MODULATION_LORA,
    };
    size_t index = 0;
    bool found = false;

    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        if (values[i] == current) {
            index = i;
            found = true;
            break;
        }
    }
    if (!found && direction < 0) {
        index = sizeof(values) / sizeof(values[0]) - 1U;
    }

    for (size_t step = 0; step < sizeof(values) / sizeof(values[0]); step++) {
        if (direction < 0) {
            index = index == 0 ? sizeof(values) / sizeof(values[0]) - 1U : index - 1U;
        } else {
            index = (index + 1U) % (sizeof(values) / sizeof(values[0]));
        }
        if ((supported & (solar_os_radio_modulations_t)values[index]) != 0) {
            return values[index];
        }
    }

    return current;
}

static void radio_tui_cycle_device(int direction)
{
    const size_t count = solar_os_radio_count();
    if (count == 0) {
        radio_tui_set_status("no radio");
        return;
    }

    if (direction < 0) {
        radio_tui.radio_index = radio_tui.radio_index == 0 ? count - 1U : radio_tui.radio_index - 1U;
    } else {
        radio_tui.radio_index = (radio_tui.radio_index + 1U) % count;
    }
    radio_tui_set_status("");
}

static void radio_tui_cycle_selected(int direction)
{
    solar_os_radio_info_t info;
    solar_os_radio_status_t status;
    solar_os_radio_config_t config;
    esp_err_t err = ESP_OK;

    if (radio_tui.selected == RADIO_TUI_ITEM_DEVICE) {
        radio_tui_cycle_device(direction);
        radio_tui_render();
        return;
    }
    if (!radio_tui_current(&info, &status)) {
        radio_tui_set_status("no radio");
        radio_tui_render();
        return;
    }

    config = status.config;
    switch ((radio_tui_item_t)radio_tui.selected) {
    case RADIO_TUI_ITEM_STATE:
        err = solar_os_radio_set_state(info.name, radio_tui_next_state(status.state, direction));
        if (err == ESP_OK) {
            radio_tui_set_status("state changed");
        }
        break;
    case RADIO_TUI_ITEM_MODULATION:
        config.modulation = radio_tui_next_modulation(config.modulation, info.modulations, direction);
        if (config.modulation != status.config.modulation) {
            radio_tui_apply_config(info.name, &config);
        }
        break;
    case RADIO_TUI_ITEM_CRC:
        config.crc_enabled = !config.crc_enabled;
        radio_tui_apply_config(info.name, &config);
        break;
    case RADIO_TUI_ITEM_VARIABLE:
        config.variable_length = !config.variable_length;
        radio_tui_apply_config(info.name, &config);
        break;
    default:
        radio_tui_set_status(radio_tui_items[radio_tui.selected].editable ? "enter edits" : "");
        break;
    }

    if (err != ESP_OK) {
        char message[RADIO_TUI_STATUS_MAX];
        snprintf(message, sizeof(message), "state failed: %s", esp_err_to_name(err));
        radio_tui_set_status(message);
    }
    radio_tui_render();
}

static void radio_tui_handle_edit_key(uint8_t key)
{
    const size_t len = strlen(radio_tui.edit_text);

    radio_tui.cursor_visible = true;
    radio_tui.last_cursor_blink_ms = 0;

    if (key == SOLAR_OS_KEY_ESCAPE) {
        radio_tui_cancel_edit();
        return;
    }
    if (key == '\r' || key == '\n') {
        radio_tui_finish_edit();
        return;
    }
    if (key == '\b' || key == 0x7fU || key == SOLAR_OS_KEY_DELETE) {
        if (len > 0) {
            radio_tui.edit_text[len - 1U] = '\0';
            radio_tui_set_status("");
            radio_tui_render();
        }
        return;
    }
    if (isprint((unsigned char)key) && len + 1U < sizeof(radio_tui.edit_text)) {
        radio_tui.edit_text[len] = (char)key;
        radio_tui.edit_text[len + 1U] = '\0';
        radio_tui_set_status("");
        radio_tui_render();
    }
}

static esp_err_t radio_tui_start(solar_os_context_t *ctx)
{
    memset(&radio_tui, 0, sizeof(radio_tui));
    radio_tui.ctx = ctx;

    const esp_err_t err = solar_os_tui_begin(&radio_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    (void)solar_os_tui_enable_diff(&radio_tui.tui, true);
    radio_tui_set_status("enter edits, arrows cycle, esc exits");
    solar_os_tui_set_cursor_visible(&radio_tui.tui, false);
    radio_tui_render();
    return ESP_OK;
}

static void radio_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&radio_tui.tui, true);
    solar_os_tui_clear(&radio_tui.tui);
    solar_os_tui_refresh(&radio_tui.tui);
    solar_os_tui_end(&radio_tui.tui);
}

static bool radio_tui_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        const uint32_t now_ms = event->data.tick_ms;
        if (radio_tui.editing) {
            if (radio_tui.last_cursor_blink_ms == 0) {
                radio_tui.last_cursor_blink_ms = now_ms;
            }
            if ((now_ms - radio_tui.last_cursor_blink_ms) >= 500U) {
                radio_tui.last_cursor_blink_ms = now_ms;
                radio_tui.cursor_visible = !radio_tui.cursor_visible;
                solar_os_tui_set_cursor_visible(&radio_tui.tui, radio_tui.cursor_visible);
            }
            return true;
        }

        if (radio_tui.last_refresh_ms == 0) {
            radio_tui.last_refresh_ms = now_ms;
            return true;
        }
        if ((now_ms - radio_tui.last_refresh_ms) >= RADIO_TUI_REFRESH_MS) {
            radio_tui.last_refresh_ms = now_ms;
            radio_tui_render();
        }
        return true;
    }

    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (radio_tui.editing) {
        radio_tui_handle_edit_key(key);
        return true;
    }

    if (key == SOLAR_OS_KEY_APP_EXIT || key == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(radio_tui.ctx);
        return true;
    }

    switch (key) {
    case SOLAR_OS_KEY_UP:
        if (radio_tui.selected > 0) {
            radio_tui.selected--;
            radio_tui_set_status("");
            radio_tui_render();
        }
        break;
    case SOLAR_OS_KEY_DOWN:
        if (radio_tui.selected + 1U < RADIO_TUI_ITEM_COUNT) {
            radio_tui.selected++;
            radio_tui_set_status("");
            radio_tui_render();
        }
        break;
    case SOLAR_OS_KEY_LEFT:
        radio_tui_cycle_selected(-1);
        break;
    case SOLAR_OS_KEY_RIGHT:
        radio_tui_cycle_selected(1);
        break;
    case '\r':
    case '\n':
        radio_tui_begin_edit();
        break;
    default:
        break;
    }

    return true;
}

static const solar_os_app_t radio_tui_app = {
    .name = "radio",
    .summary = "Packet radio control",
    .start = radio_tui_start,
    .stop = radio_tui_stop,
    .event = radio_tui_event,
};

esp_err_t solar_os_shell_launch_radio_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &radio_tui_app, 0, NULL);
}
