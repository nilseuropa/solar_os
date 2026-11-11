#include "usb_hid_report.h"

#include <string.h>

#define REPORT_BITS_MAX (SOLAR_OS_USB_HID_REPORT_BYTES_MAX * 8U)
#define USAGE_MAX 32U
#define COLLECTION_MAX 16U
#define GLOBAL_STACK_MAX 4U

typedef struct {
    uint32_t page, size, count, maximum;
    int64_t minimum;
    uint8_t id, maximum_bits;
} globals_t;

typedef struct {
    uint32_t usages[USAGE_MAX], minimum, maximum;
    size_t count;
    bool has_minimum, has_maximum;
} locals_t;

typedef struct { uint32_t usage; uint16_t instance; } application_t;

static int64_t signed_value(uint32_t value, unsigned bits)
{
    int64_t result = value;
    if (bits != 0 && (value & (UINT32_C(1) << (bits - 1)))) result -= INT64_C(1) << bits;
    return result;
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

esp_err_t solar_os_usb_hid_report_parse(const uint8_t *data, size_t length,
    solar_os_usb_hid_field_callback_t callback, void *context,
    solar_os_usb_hid_report_info_t *info)
{
    if (data == NULL || callback == NULL || info == NULL) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    if (length == 0 || length > 4096) return ESP_ERR_INVALID_SIZE;
    globals_t global = {0}, stack[GLOBAL_STACK_MAX];
    locals_t local = {0};
    uint16_t offsets[256] = {0}, instance = 0;
    application_t applications[COLLECTION_MAX];
    bool numbered = false;
    size_t depth = 0, stack_depth = 0;
    for (size_t cursor = 0; cursor < length;) {
        const uint8_t prefix = data[cursor++];
        if (prefix == 0xfe) return ESP_ERR_NOT_SUPPORTED;
        const unsigned bytes = (prefix & 3U) == 3 ? 4 : prefix & 3U;
        if (bytes > length - cursor) return ESP_ERR_INVALID_RESPONSE;
        uint32_t value = 0;
        for (unsigned i = 0; i < bytes; i++) value |= (uint32_t)data[cursor++] << (8U * i);
        const unsigned type = (prefix >> 2) & 3U, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0:
                if (value > 0xffff) return ESP_ERR_NOT_SUPPORTED;
                global.page = value;
                break;
            case 1: global.minimum = signed_value(value, bytes * 8U); break;
            case 2: global.maximum = value; global.maximum_bits = (uint8_t)(bytes * 8U); break;
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
            default: break;
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
            case 10: return ESP_ERR_NOT_SUPPORTED;
            default: break;
            }
        } else if (type == 0) {
            if (local.has_minimum != local.has_maximum ||
                (local.has_minimum && (local.maximum < local.minimum ||
                 (local.minimum >> 16) != (local.maximum >> 16)))) return ESP_ERR_INVALID_RESPONSE;
            if (tag == 10) {
                if (depth == COLLECTION_MAX) return ESP_ERR_NOT_SUPPORTED;
                application_t application = depth == 0 ? (application_t){0} : applications[depth - 1];
                if (value == 1) application = (application_t){usage_at(&local, 0), ++instance};
                applications[depth++] = application;
            } else if (tag == 12) {
                if (depth == 0) return ESP_ERR_INVALID_RESPONSE;
                depth--;
            } else if (tag == 8) {
                if (global.size == 0 || global.count == 0) return ESP_ERR_INVALID_RESPONSE;
                const uint32_t bits = global.size * global.count;
                const uint16_t start = offsets[global.id];
                if (bits > REPORT_BITS_MAX - start) return ESP_ERR_NOT_SUPPORTED;
                offsets[global.id] = (uint16_t)(start + bits);
                if (depth != 0 && (value & 3U) == 2 && !(value & 0x100U)) {
                    const application_t application = applications[depth - 1];
                    for (uint32_t i = 0; i < global.count; i++) {
                        const solar_os_usb_hid_field_t field = {
                            .application_usage = application.usage, .application = application.instance,
                            .usage = usage_at(&local, i), .flags = value,
                            .report_id = global.id, .offset = (uint16_t)(start + i * global.size),
                            .bits = (uint8_t)global.size, .logical_min = global.minimum,
                            .logical_max = global.minimum < 0 ? signed_value(global.maximum, global.maximum_bits) : global.maximum,
                        };
                        const esp_err_t ret = callback(&field, context);
                        if (ret != ESP_OK) return ret;
                    }
                }
            }
            memset(&local, 0, sizeof(local));
        }
    }
    if (depth != 0 || stack_depth != 0 || (numbered && offsets[0] != 0)) return ESP_ERR_INVALID_RESPONSE;
    for (unsigned id = 0; id < 256; id++) {
        if (offsets[id] == 0) continue;
        const unsigned bytes = (offsets[id] + 7U) / 8U + (numbered ? 1U : 0U);
        if (bytes > SOLAR_OS_USB_HID_REPORT_BYTES_MAX) return ESP_ERR_NOT_SUPPORTED;
        info->report_bytes[id] = (uint8_t)bytes;
    }
    info->numbered = numbered;
    return ESP_OK;
}

esp_err_t solar_os_usb_hid_field_read(const uint8_t *payload, size_t length,
    const solar_os_usb_hid_field_t *field, int64_t *value)
{
    if (payload == NULL || field == NULL || value == NULL || field->bits == 0 || field->bits > 32) return ESP_ERR_INVALID_ARG;
    if ((size_t)field->offset + field->bits > length * 8U) return ESP_ERR_INVALID_SIZE;
    uint32_t raw = 0;
    for (unsigned i = 0; i < field->bits; i++) {
        const unsigned bit = field->offset + i;
        raw |= (uint32_t)((payload[bit / 8U] >> (bit % 8U)) & 1U) << i;
    }
    *value = field->logical_min < 0 ? signed_value(raw, field->bits) : raw;
    return ESP_OK;
}
