#include "usb_hid_keyboard_report.h"

#include <string.h>

typedef struct {
    solar_os_usb_hid_keyboard_layout_t *layout;
    uint16_t application;
} keyboard_parser_t;

static esp_err_t collect_keyboard_field(const solar_os_usb_hid_field_t *field, void *context)
{
    if (field->application_usage != 0x10006 || (field->usage >> 16) != 7) return ESP_OK;
    keyboard_parser_t *parser = context;
    if (parser->application && parser->application != field->application) return ESP_OK;
    solar_os_usb_hid_keyboard_layout_t *layout = parser->layout;
    if (parser->application && layout->report_id != field->report_id) return ESP_ERR_NOT_SUPPORTED;
    if (field->flags & 4U) return ESP_ERR_NOT_SUPPORTED; /* Keys are absolute. */
    if (field->flags & 2U) {
        /* Bitmap/NKRO keys need a different normalization policy. */
        if (field->usage < 0x700e0 || field->usage > 0x700e7 || field->bits != 1 ||
            field->logical_min != 0 || field->logical_max != 1) return ESP_ERR_NOT_SUPPORTED;
        const unsigned bit = field->usage - 0x700e0;
        if (layout->modifiers & (1U << bit)) return ESP_ERR_NOT_SUPPORTED;
        layout->modifiers |= 1U << bit;
        layout->modifier_offsets[bit] = field->offset;
    } else {
        /* Usage values map directly to their contiguous Keyboard usage range. */
        if (layout->key_count || field->usage != 0x70000 || field->usage_max <= 0x70000 ||
            field->usage_max > 0x700ff || field->logical_min != 0 || field->logical_max <= 0 ||
            field->logical_max != (int64_t)(field->usage_max & 0xffffU) ||
            field->bits != 8 || field->count > 6) return ESP_ERR_NOT_SUPPORTED;
        layout->key_offset = field->offset;
        layout->key_count = (uint8_t)field->count;
        layout->key_max = (uint8_t)field->logical_max;
    }
    parser->application = field->application;
    layout->report_id = field->report_id;
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_keyboard_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_keyboard_layout_t *layout)
{
    if (layout == NULL) return ESP_ERR_INVALID_ARG;
    memset(layout, 0, sizeof(*layout));
    keyboard_parser_t parser = {.layout = layout};
    solar_os_usb_hid_report_info_t info;
    const esp_err_t ret = solar_os_usb_hid_report_parse(data, length, collect_keyboard_field, &parser, &info);
    if (ret != ESP_OK || !layout->key_count) {
        memset(layout, 0, sizeof(*layout));
        return ret == ESP_OK ? ESP_ERR_NOT_SUPPORTED : ret;
    }
    layout->numbered = info.numbered;
    layout->report_bytes = info.report_bytes[layout->report_id];
    layout->valid = true;
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_keyboard_decode(const solar_os_usb_hid_keyboard_layout_t *layout,
    const uint8_t *data, size_t length, uint8_t report[8])
{
    if (layout == NULL || !layout->valid || data == NULL || report == NULL ||
        !layout->key_count || layout->key_count > 6) return ESP_ERR_INVALID_ARG;
    if (layout->numbered) {
        if (!length) return ESP_ERR_INVALID_SIZE;
        if (data[0] != layout->report_id) return ESP_ERR_NOT_FOUND;
    }
    if (length < layout->report_bytes) return ESP_ERR_INVALID_SIZE;
    if (layout->numbered) { data++; length--; }
    memset(report, 0, 8);
    for (unsigned bit = 0; bit < 8; bit++) {
        if (!(layout->modifiers & (1U << bit))) continue;
        const solar_os_usb_hid_field_t field = {.offset = layout->modifier_offsets[bit], .bits = 1};
        int64_t value;
        const esp_err_t ret = solar_os_usb_hid_field_read(data, length, &field, &value);
        if (ret != ESP_OK) return ret;
        if (value) report[0] |= 1U << bit;
    }
    for (unsigned i = 0; i < layout->key_count; i++) {
        const solar_os_usb_hid_field_t field = {.offset = (uint16_t)(layout->key_offset + 8U * i), .bits = 8};
        int64_t value;
        const esp_err_t ret = solar_os_usb_hid_field_read(data, length, &field, &value);
        if (ret != ESP_OK) return ret;
        if (value > layout->key_max) return ESP_ERR_INVALID_RESPONSE;
        report[i + 2] = (uint8_t)value;
    }
    return ESP_OK;
}
