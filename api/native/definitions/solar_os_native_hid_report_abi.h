#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_hid_types_v1.h"
#define SOLAR_OS_NATIVE_HID_REPORT_SERVICE "hid.report"
#define SOLAR_OS_NATIVE_HID_REPORT_ABI 1U

typedef struct {
    uint32_t report_streams, report_keys, descriptor_report_bytes;
} solar_os_native_hid_report_limits_v1_t;

/* Shared kernels over an opaque owned tracker and borrowed input/output values.
 * Creation allocates one tracker; processing, reset and descriptor parsing do
 * not allocate or retain buffers/callbacks. Serialize each tracker's operations.
 * Close reports BUSY while in use. Trackers created in an application callback
 * are also reclaimed by application cleanup; otherwise close explicitly.
 * Descriptor support covers Keyboard/Keypad Application collections with
 * byte-aligned modifier bitmaps/key arrays, bounded by the reported limits.
 * Unsupported/malformed layouts return false. Report bodies exclude transport
 * IDs. Update returns false on invalid handles, reports or stream exhaustion;
 * output is unchanged on false. Reset releases all tracked modifiers/keys. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*limits)(solar_os_native_hid_report_limits_v1_t *out);
    int (*create)(solar_os_native_handle_t *out);
    int (*close)(solar_os_native_handle_t tracker);
    int (*reset)(solar_os_native_handle_t tracker);
    bool (*update)(solar_os_native_handle_t tracker, uint8_t map_index, uint16_t report_id,
        const uint8_t *data, size_t length, solar_os_hid_keyboard_report_state_t *state);
    bool (*update_with_layout)(solar_os_native_handle_t tracker, uint8_t map_index,
        uint16_t report_id, const uint8_t *data, size_t length,
        const solar_os_hid_keyboard_report_layout_t *layout, solar_os_hid_keyboard_report_state_t *state);
    bool (*has_new_press)(uint8_t previous_modifiers,
        const uint8_t previous_keys[SOLAR_OS_HID_KEYBOARD_REPORT_KEYS], const solar_os_hid_keyboard_report_state_t *state);
    bool (*descriptor_map)(const uint8_t *data, size_t length, bool keyboard_ids[256]);
    bool (*descriptor_layout)(const uint8_t *data, size_t length, uint8_t report_id,
        solar_os_hid_keyboard_report_layout_t *layout);
} solar_os_native_hid_report_api_v1_t;
