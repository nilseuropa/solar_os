#include "usb_hid_joystick_report.h"

#include <string.h>

typedef struct {
    solar_os_usb_hid_joystick_layout_t *layout;
    uint16_t application;
} joystick_parser_t;

static esp_err_t collect_joystick_field(const solar_os_usb_hid_field_t *field, void *context)
{
    if (!(field->flags & 2U) ||
        (field->application_usage != 0x10004 && field->application_usage != 0x10005)) return ESP_OK;
    joystick_parser_t *parser = context;
    if (parser->application != 0 && parser->application != field->application) return ESP_OK;
    solar_os_usb_hid_joystick_layout_t *layout = parser->layout;
    solar_os_usb_hid_joystick_field_t *target = NULL;
    solar_os_usb_hid_joystick_range_t *range = NULL;
    if (field->usage >= 0x10030 && field->usage <= 0x10035 && !(field->flags & 4U)) {
        range = &layout->axes[field->usage - 0x10030];
    } else if (field->usage == 0x10039 && !(field->flags & 4U)) {
        const int64_t positions = field->logical_max - field->logical_min + 1;
        if (positions != 4 && positions != 8) return ESP_OK;
        range = &layout->hat;
    } else if (field->usage >= 0x90001 && field->usage <= 0x90010) {
        target = &layout->buttons[field->usage - 0x90001];
    } else if (field->usage >= 0x10090 && field->usage <= 0x10093) {
        static const uint8_t indices[] = {0, 1, 3, 2};
        target = &layout->dpad[indices[field->usage - 0x10090]];
    } else return ESP_OK;
    if (range != NULL) {
        if (field->logical_max <= field->logical_min) return ESP_ERR_INVALID_RESPONSE;
        range->minimum = field->logical_min;
        range->maximum = field->logical_max;
        target = &range->field;
    }
    if (target->bits != 0) return ESP_ERR_NOT_SUPPORTED;
    parser->application = field->application;
    *target = (solar_os_usb_hid_joystick_field_t){
        .offset = field->offset, .bits = field->bits, .report_id = field->report_id,
    };
    for (unsigned i = 0; i < layout->report_count; i++) {
        if (layout->reports[i].id == field->report_id) return ESP_OK;
    }
    if (layout->report_count == SOLAR_OS_USB_HID_JOYSTICK_REPORT_MAX) return ESP_ERR_NOT_SUPPORTED;
    layout->reports[layout->report_count++].id = field->report_id;
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_joystick_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_joystick_layout_t *layout)
{
    if (layout == NULL) return ESP_ERR_INVALID_ARG;
    memset(layout, 0, sizeof(*layout));
    joystick_parser_t parser = {.layout = layout};
    solar_os_usb_hid_report_info_t info;
    const esp_err_t ret = solar_os_usb_hid_report_parse(data, length, collect_joystick_field, &parser, &info);
    if (ret != ESP_OK || layout->report_count == 0) {
        memset(layout, 0, sizeof(*layout));
        return ret == ESP_OK ? ESP_ERR_NOT_SUPPORTED : ret;
    }
    for (unsigned i = 0; i < layout->report_count; i++) {
        layout->reports[i].bytes = info.report_bytes[layout->reports[i].id];
    }
    layout->numbered = info.numbered;
    layout->valid = true;
    return ESP_OK;
}

static esp_err_t read_control(const uint8_t *data, size_t length,
    solar_os_usb_hid_joystick_field_t field, int64_t minimum, int64_t *value)
{
    const solar_os_usb_hid_field_t generic = {
        .offset = field.offset, .bits = field.bits, .logical_min = minimum,
    };
    return solar_os_usb_hid_field_read(data, length, &generic, value);
}

static int16_t normalize(int64_t value, int64_t minimum, int64_t maximum)
{
    if (value <= minimum) return -32767;
    if (value >= maximum) return 32767;
    const int64_t center = minimum + (maximum - minimum + 1) / 2;
    if (value < center) return (int16_t)((value - center) * 32767 / (center - minimum));
    if (value > center) return (int16_t)((value - center) * 32767 / (maximum - center));
    return 0;
}

esp_err_t solar_os_usb_hid_joystick_decode(const solar_os_usb_hid_joystick_layout_t *layout,
    const uint8_t *data, size_t length, solar_os_usb_hid_joystick_sample_t *sample)
{
    if (layout == NULL || !layout->valid || data == NULL || sample == NULL ||
        layout->report_count == 0 || layout->report_count > SOLAR_OS_USB_HID_JOYSTICK_REPORT_MAX) return ESP_ERR_INVALID_ARG;
    memset(sample, 0, sizeof(*sample));
    if (length == 0) return ESP_ERR_INVALID_SIZE;
    const uint8_t id = layout->numbered ? data[0] : 0;
    unsigned index = 0;
    while (index < layout->report_count && layout->reports[index].id != id) index++;
    if (index == layout->report_count) return ESP_ERR_NOT_FOUND;
    if (length < layout->reports[index].bytes) return ESP_ERR_INVALID_SIZE;
    if (layout->numbered) { data++; length--; }
    for (unsigned i = 0; i < SOLAR_OS_USB_HID_JOYSTICK_AXIS_MAX; i++) {
        const solar_os_usb_hid_joystick_range_t *axis = &layout->axes[i];
        if (axis->field.bits == 0 || axis->field.report_id != id) continue;
        if (axis->maximum <= axis->minimum) return ESP_ERR_INVALID_ARG;
        int64_t value;
        const esp_err_t ret = read_control(data, length, axis->field, axis->minimum, &value);
        if (ret != ESP_OK) return ret;
        sample->axes[i] = normalize(value, axis->minimum, axis->maximum);
        sample->axes_mask |= 1U << i;
    }
    for (unsigned i = 0; i < SOLAR_OS_USB_HID_JOYSTICK_BUTTON_MAX; i++) {
        const solar_os_usb_hid_joystick_field_t field = layout->buttons[i];
        if (field.bits == 0 || field.report_id != id) continue;
        int64_t value;
        const esp_err_t ret = read_control(data, length, field, 0, &value);
        if (ret != ESP_OK) return ret;
        sample->buttons_mask |= 1U << i;
        if (value != 0) sample->buttons |= 1U << i;
    }
    const solar_os_usb_hid_joystick_range_t *hat = &layout->hat;
    if (hat->field.bits != 0 && hat->field.report_id == id) {
        static const uint8_t directions[8] = {1, 9, 8, 10, 2, 6, 4, 5};
        int64_t value;
        const esp_err_t ret = read_control(data, length, hat->field, hat->minimum, &value);
        if (ret != ESP_OK) return ret;
        sample->directions_mask = 15;
        if (value >= hat->minimum && value <= hat->maximum) {
            const int64_t positions = hat->maximum - hat->minimum + 1;
            if (positions != 4 && positions != 8) return ESP_ERR_INVALID_ARG;
            sample->directions = directions[(value - hat->minimum) * (positions == 4 ? 2 : 1)];
        }
    } else if (hat->field.bits == 0) {
        for (unsigned i = 0; i < 4; i++) {
            const solar_os_usb_hid_joystick_field_t field = layout->dpad[i];
            if (field.bits == 0 || field.report_id != id) continue;
            int64_t value;
            const esp_err_t ret = read_control(data, length, field, 0, &value);
            if (ret != ESP_OK) return ret;
            sample->directions_mask |= 1U << i;
            if (value != 0) sample->directions |= 1U << i;
        }
    }
    return ESP_OK;
}
