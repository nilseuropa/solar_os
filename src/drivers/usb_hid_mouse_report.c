#include "usb_hid_mouse_report.h"
#include "usb_hid_report.h"

#include <string.h>

#define MOUSE_REPORT_MAX 8U

typedef struct {
    size_t count;
    solar_os_usb_hid_mouse_layout_t candidates[MOUSE_REPORT_MAX];
} mouse_parser_t;

static esp_err_t collect_mouse_field(const solar_os_usb_hid_field_t *field, void *context)
{
    if (!(field->flags & 2U) || field->application_usage != 0x10002 || field->bits > 16) return ESP_OK;
    const bool button = field->usage >= 0x90001 && field->usage <= 0x90003;
    const bool axis = (field->usage == 0x10030 || field->usage == 0x10031) &&
        (field->flags & 4U) && field->logical_min < 0;
    if (!button && !axis) return ESP_OK;
    mouse_parser_t *parser = context;
    size_t index = 0;
    while (index < parser->count && parser->candidates[index].report_id != field->report_id) index++;
    if (index == parser->count) {
        if (parser->count == MOUSE_REPORT_MAX) return ESP_ERR_NOT_SUPPORTED;
        parser->candidates[parser->count++].report_id = field->report_id;
    }
    solar_os_usb_hid_mouse_layout_t *candidate = &parser->candidates[index];
    solar_os_usb_hid_mouse_field_t *target = button ? &candidate->buttons[field->usage - 0x90001] :
        field->usage == 0x10030 ? &candidate->x : &candidate->y;
    if (target->bits != 0) return ESP_ERR_NOT_SUPPORTED;
    *target = (solar_os_usb_hid_mouse_field_t){.offset = field->offset, .bits = field->bits};
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_mouse_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_layout_t *layout)
{
    if (layout == NULL) return ESP_ERR_INVALID_ARG;
    memset(layout, 0, sizeof(*layout));
    mouse_parser_t parser = {0};
    solar_os_usb_hid_report_info_t info;
    const esp_err_t ret = solar_os_usb_hid_report_parse(data, length, collect_mouse_field, &parser, &info);
    if (ret != ESP_OK) return ret;
    for (size_t i = 0; i < parser.count; i++) {
        solar_os_usb_hid_mouse_layout_t *candidate = &parser.candidates[i];
        if (candidate->x.bits && candidate->y.bits && candidate->buttons[0].bits) {
            candidate->valid = true;
            candidate->numbered = info.numbered;
            candidate->report_bytes = info.report_bytes[candidate->report_id];
            *layout = *candidate;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t read_mouse_field(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_field_t field, bool sign, int64_t *value)
{
    *value = 0;
    if (field.bits == 0) return ESP_OK;
    if (field.bits > 16) return ESP_ERR_INVALID_ARG;
    const solar_os_usb_hid_field_t generic = {
        .offset = field.offset, .bits = field.bits, .logical_min = sign ? -1 : 0,
    };
    return solar_os_usb_hid_field_read(data, length, &generic, value);
}

esp_err_t solar_os_usb_hid_mouse_decode(const solar_os_usb_hid_mouse_layout_t *layout,
    const uint8_t *data, size_t length, uint8_t *buttons, int16_t *x, int16_t *y)
{
    if (layout == NULL || !layout->valid || data == NULL || buttons == NULL || x == NULL || y == NULL ||
        layout->x.bits == 0 || layout->y.bits == 0) return ESP_ERR_INVALID_ARG;
    if (layout->numbered) {
        if (length == 0) return ESP_ERR_INVALID_SIZE;
        if (data[0] != layout->report_id) return ESP_ERR_NOT_FOUND;
    }
    if (length < layout->report_bytes) return ESP_ERR_INVALID_SIZE;
    if (layout->numbered) { data++; length--; }
    int64_t raw_x, raw_y;
    esp_err_t ret = read_mouse_field(data, length, layout->x, true, &raw_x);
    if (ret == ESP_OK) ret = read_mouse_field(data, length, layout->y, true, &raw_y);
    if (ret != ESP_OK) return ret;
    uint8_t pressed = 0;
    for (unsigned i = 0; i < 3; i++) {
        int64_t value;
        ret = read_mouse_field(data, length, layout->buttons[i], false, &value);
        if (ret != ESP_OK) return ret;
        if (value != 0) pressed |= 1U << i;
    }
    *buttons = pressed;
    *x = (int16_t)raw_x;
    *y = (int16_t)raw_y;
    return ESP_OK;
}
