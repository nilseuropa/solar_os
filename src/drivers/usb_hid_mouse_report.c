#include "usb_hid_mouse_report.h"

#include <string.h>

#define REPORT_BITS_MAX 512U
#define MOUSE_REPORT_MAX 8U
#define USAGE_MAX 32U
#define COLLECTION_MAX 16U
#define GLOBAL_STACK_MAX 4U

typedef struct {
    uint32_t page, size, count;
    int32_t logical_min;
    uint8_t id;
} globals_t;

typedef struct {
    uint32_t usages[USAGE_MAX], minimum, maximum;
    size_t count;
    bool has_minimum, has_maximum;
} locals_t;

static int32_t signed_value(uint32_t value, unsigned bits)
{
    int64_t result = value;
    if (bits != 0 && (value & (UINT32_C(1) << (bits - 1)))) result -= INT64_C(1) << bits;
    return (int32_t)result;
}

static uint32_t usage_at(const locals_t *local, uint32_t index)
{
    if (local->has_minimum && local->has_maximum) {
        const uint32_t count = local->maximum - local->minimum;
        return local->minimum + (index < count ? index : count);
    }
    if (local->count == 0) return 0;
    return local->usages[index < local->count ? index : local->count - 1];
}

esp_err_t solar_os_usb_hid_mouse_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_layout_t *layout)
{
    if (data == NULL || layout == NULL) return ESP_ERR_INVALID_ARG;
    memset(layout, 0, sizeof(*layout));
    if (length == 0 || length > 4096) return ESP_ERR_INVALID_SIZE;
    globals_t global = {0}, stack[GLOBAL_STACK_MAX];
    locals_t local = {0};
    uint16_t offsets[256] = {0};
    bool mouse[COLLECTION_MAX], numbered = false;
    size_t depth = 0, stack_depth = 0, count = 0;
    solar_os_usb_hid_mouse_layout_t candidates[MOUSE_REPORT_MAX] = {0};
    for (size_t cursor = 0; cursor < length;) {
        const uint8_t prefix = data[cursor++];
        if (prefix == 0xfe) return ESP_ERR_NOT_SUPPORTED; /* Reserved long items. */
        const unsigned bytes = (prefix & 3U) == 3 ? 4 : prefix & 3U;
        if (bytes > length - cursor) return ESP_ERR_INVALID_RESPONSE;
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; i++) value |= (uint32_t)data[cursor++] << (8U * i);
        const unsigned type = (prefix >> 2) & 3U, tag = prefix >> 4;
        if (type == 1) { /* Globals survive Main items; Push/Pop restore all used globals. */
            switch (tag) {
            case 0:
                if (value > 0xffff) return ESP_ERR_NOT_SUPPORTED;
                global.page = value;
                break;
            case 1: global.logical_min = signed_value(value, bytes * 8U); break;
            case 7:
                if (value > 32) return ESP_ERR_NOT_SUPPORTED;
                global.size = value;
                break;
            case 8:
                if (value == 0 || value > 255) return ESP_ERR_INVALID_RESPONSE;
                global.id = (uint8_t)value;
                numbered = true;
                break;
            case 9:
                if (value > REPORT_BITS_MAX) return ESP_ERR_NOT_SUPPORTED;
                global.count = value;
                break;
            case 10:
                if (stack_depth == GLOBAL_STACK_MAX) return ESP_ERR_NOT_SUPPORTED;
                stack[stack_depth++] = global;
                break;
            case 11:
                if (stack_depth == 0) return ESP_ERR_INVALID_RESPONSE;
                global = stack[--stack_depth];
                break;
            default: break; /* Physical ranges and units do not change wire offsets. */
            }
        } else if (type == 2) {
            const uint32_t usage = bytes == 4 ? value : (global.page << 16) | value;
            switch (tag) {
            case 0:
                if (local.count == USAGE_MAX || local.has_minimum) return ESP_ERR_NOT_SUPPORTED;
                local.usages[local.count++] = usage;
                break;
            case 1:
                if (local.count != 0) return ESP_ERR_NOT_SUPPORTED;
                local.minimum = usage;
                local.has_minimum = true;
                break;
            case 2: local.maximum = usage; local.has_maximum = true; break;
            case 10: return ESP_ERR_NOT_SUPPORTED; /* Alternative usage sets. */
            default: break;
            }
        } else if (type == 0) {
            if (local.has_minimum != local.has_maximum ||
                (local.has_minimum && (local.maximum < local.minimum ||
                 (local.minimum >> 16) != (local.maximum >> 16)))) return ESP_ERR_INVALID_RESPONSE;
            if (tag == 10) { /* Collection */
                if (depth == COLLECTION_MAX) return ESP_ERR_NOT_SUPPORTED;
                const bool parent = depth != 0 && mouse[depth - 1];
                mouse[depth++] = value == 1 ? usage_at(&local, 0) == 0x10002 : parent;
            } else if (tag == 12) {
                if (depth == 0) return ESP_ERR_INVALID_RESPONSE;
                depth--;
            } else if (tag == 8) { /* Input: each report ID has an independent bit cursor. */
                if (global.size == 0 || global.count == 0) return ESP_ERR_INVALID_RESPONSE;
                const uint32_t bits = global.size * global.count;
                const uint16_t start = offsets[global.id];
                if (bits > REPORT_BITS_MAX - start) return ESP_ERR_NOT_SUPPORTED;
                offsets[global.id] = (uint16_t)(start + bits);
                if (depth != 0 && mouse[depth - 1] && (value & 3U) == 2 && !(value & 0x100U)) {
                    for (uint32_t i = 0; i < global.count; i++) {
                        const uint32_t usage = usage_at(&local, i);
                        const bool button = usage >= 0x90001 && usage <= 0x90003;
                        const bool axis = (usage == 0x10030 || usage == 0x10031) &&
                            (value & 4U) && global.logical_min < 0;
                        if ((!button && !axis) || global.size > 16) continue;
                        size_t index = 0;
                        while (index < count && candidates[index].report_id != global.id) index++;
                        if (index == count) {
                            if (count == MOUSE_REPORT_MAX) return ESP_ERR_NOT_SUPPORTED;
                            candidates[count++].report_id = global.id;
                        }
                        solar_os_usb_hid_mouse_layout_t *candidate = &candidates[index];
                        solar_os_usb_hid_mouse_field_t *field = button ? &candidate->buttons[usage - 0x90001] :
                            usage == 0x10030 ? &candidate->x : &candidate->y;
                        if (field->bits != 0) return ESP_ERR_NOT_SUPPORTED;
                        *field = (solar_os_usb_hid_mouse_field_t){
                            .offset = (uint16_t)(start + i * global.size), .bits = (uint8_t)global.size,
                        };
                    }
                }
            }
            /* Every Main item, including Output/Feature/Collection, clears locals. */
            memset(&local, 0, sizeof(local));
        }
    }
    if (depth != 0 || stack_depth != 0 || (numbered && offsets[0] != 0)) return ESP_ERR_INVALID_RESPONSE;
    for (size_t i = 0; i < count; i++) {
        solar_os_usb_hid_mouse_layout_t *candidate = &candidates[i];
        const unsigned bytes = (offsets[candidate->report_id] + 7U) / 8U + (numbered ? 1U : 0U);
        if (candidate->x.bits && candidate->y.bits && candidate->buttons[0].bits && bytes <= 64) {
            candidate->valid = true;
            candidate->numbered = numbered;
            candidate->report_bytes = (uint8_t)bytes;
            *layout = *candidate;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static bool read_field(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_field_t field, uint32_t *value)
{
    *value = 0;
    if (field.bits == 0) return true;
    if (field.bits > 16 || (size_t)field.offset + field.bits > length * 8U) return false;
    for (unsigned i = 0; i < field.bits; i++) {
        const unsigned bit = field.offset + i;
        *value |= (uint32_t)((data[bit / 8U] >> (bit % 8U)) & 1U) << i;
    }
    return true;
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
    uint32_t raw_x, raw_y;
    if (!read_field(data, length, layout->x, &raw_x) ||
        !read_field(data, length, layout->y, &raw_y)) return ESP_ERR_INVALID_SIZE;
    uint8_t pressed = 0;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t value;
        if (!read_field(data, length, layout->buttons[i], &value)) return ESP_ERR_INVALID_SIZE;
        if (value != 0) pressed |= 1U << i;
    }
    *buttons = pressed;
    *x = (int16_t)signed_value(raw_x, layout->x.bits);
    *y = (int16_t)signed_value(raw_y, layout->y.bits);
    return ESP_OK;
}
