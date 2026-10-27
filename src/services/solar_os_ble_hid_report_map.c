#include "solar_os_ble_hid_report_map.h"
#include <string.h>

static bool parse_map(const uint8_t *data, size_t len, bool ids[256],
    uint8_t report_id, solar_os_hid_keyboard_report_layout_t *layout)
{
    struct globals { uint32_t page, size, count; uint8_t id; } global = {0}, stack[8];
    bool collections[16];
    size_t globals_depth = 0, depth = 0, offset = 0;
    uint32_t usage = 0, usage_page = 0, usage_min = 0, usage_max = 0;
    uint64_t input_bits = 0;
    bool has_usage = false, has_modifiers = false, has_keys = false;
    bool supported = true;
    solar_os_hid_keyboard_report_layout_t found = {0};
    if (!data || !len) return false;
    if (ids) memset(ids, 0, 256 * sizeof(*ids));
    while (offset < len) {
        uint8_t prefix = data[offset++];
        if (prefix == 0xfe) return false; /* Long items are not used by HOGP keyboards. */
        size_t n = prefix & 3;
        if (n == 3) n = 4;
        if (n > len - offset) return false;
        uint32_t value = 0;
        for (size_t i = 0; i < n; ++i) value |= (uint32_t)data[offset++] << (8 * i);
        unsigned type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) {
            switch (tag) {
            case 0: global.page = value; break;
            case 7: global.size = value; break;
            case 8:
                if (!value || value > 255) return false;
                global.id = value;
                break;
            case 9: global.count = value; break;
            case 10:
                if (n || globals_depth == 8) return false;
                stack[globals_depth++] = global;
                break;
            case 11:
                if (n || !globals_depth) return false;
                global = stack[--globals_depth];
                break;
            default: break;
            }
        } else if (type == 2) {
            if (tag == 0) {
                usage = value & 0xffff;
                usage_page = n == 4 ? value >> 16 : global.page;
                has_usage = true;
            }
            if (tag == 1) usage_min = value;
            if (tag == 2) usage_max = value;
            if (tag == 10) return false; /* Unsupported local delimiter sets. */
        } else if (type == 0) {
            switch (tag) {
            case 10: /* Collection */
                if (n != 1 || depth == 16) return false;
                collections[depth] = (depth && collections[depth - 1]) ||
                    (value == 1 && has_usage && usage_page == 1 && (usage == 6 || usage == 7));
                ++depth;
                break;
            case 12: /* End Collection */
                if (n || !depth) return false;
                --depth;
                break;
            case 8: /* Input */
                if (!n || !depth) return false;
                if (collections[depth - 1] && ids) ids[global.id] = true;
                if (layout && global.id == report_id) {
                    const uint64_t bits = (uint64_t)global.size * global.count;
                    /* Notifications are bounded to 64 bytes by the HID client. */
                    if (bits > 512U || input_bits + bits > 512U) return false;
                    if (collections[depth - 1] && global.page == 7 && !(value & 1U)) {
                        if ((value & 2U) && global.size == 1 && global.count == 8 &&
                            usage_min == 0xe0 && usage_max == 0xe7 && !(input_bits % 8U)) {
                            if (has_modifiers) supported = false;
                            found.modifiers_offset = input_bits / 8U;
                            has_modifiers = true;
                        } else if (!(value & 2U) && global.size == 8 && global.count &&
                                   !(input_bits % 8U)) {
                            if (has_keys) supported = false;
                            found.keys_offset = input_bits / 8U;
                            found.key_count = global.count;
                            has_keys = true;
                        } else {
                            /* Other keyboard fields (e.g. NKRO bitmaps) need
                             * their own decoder; do not invent byte offsets. */
                            supported = false;
                        }
                    }
                    input_bits += bits;
                }
                break;
            default: break;
            }
            has_usage = false;
            usage_min = usage_max = 0;
        } else return false;
    }
    if (depth || globals_depth) return false;
    if (layout) {
        if (!supported || !has_modifiers || !has_keys) return false;
        found.report_size = (input_bits + 7U) / 8U;
        *layout = found;
    }
    return true;
}

bool solar_os_ble_hid_report_map(const uint8_t *data, size_t len, bool ids[256])
{
    return ids && parse_map(data, len, ids, 0, NULL);
}

bool solar_os_ble_hid_report_layout(const uint8_t *data, size_t len,
    uint8_t report_id, solar_os_hid_keyboard_report_layout_t *layout)
{
    if (!layout) return false;
    memset(layout, 0, sizeof(*layout));
    return parse_map(data, len, NULL, report_id, layout);
}
