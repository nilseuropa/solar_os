#include "solar_os_native_hid_report.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_hid_report_abi.h"
#if SOLAR_OS_PACKAGE_CORE_RUNTIME
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_hid_keyboard_report.h"
#include "solar_os_hid_report_descriptor.h"
static int report_limits(solar_os_native_hid_report_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_hid_report_limits_v1_t){
        .report_streams = SOLAR_OS_HID_KEYBOARD_REPORT_STREAMS,
        .report_keys = SOLAR_OS_HID_KEYBOARD_REPORT_KEYS,
        .descriptor_report_bytes = SOLAR_OS_HID_DESCRIPTOR_REPORT_BYTES_MAX};
    return SOLAR_OS_NATIVE_OK;
}
static int report_destroy(void *state)
{ solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK; }
static int report_create(solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    solar_os_hid_keyboard_report_tracker_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_TRANSIENT, "native.hid.report");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const solar_os_app_t *owner = solar_os_application_descriptor();
    int result = owner ? solar_os_native_handle_create_owned(SOLAR_OS_NATIVE_HANDLE_HID_REPORT,
        state, report_destroy, owner, out) : solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_HID_REPORT,
        state, report_destroy, out);
    if (result) solar_os_memory_free(state);
    return result;
}
static int report_close(solar_os_native_handle_t tracker)
{ return solar_os_native_handle_close_kind(tracker, SOLAR_OS_NATIVE_HANDLE_HID_REPORT); }
static int report_reset(solar_os_native_handle_t tracker)
{
    solar_os_hid_keyboard_report_tracker_t *state = solar_os_native_handle_acquire(tracker, SOLAR_OS_NATIVE_HANDLE_HID_REPORT);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_hid_keyboard_report_reset(state);
    solar_os_native_handle_release(tracker);
    return SOLAR_OS_NATIVE_OK;
}
static bool report_update(solar_os_native_handle_t tracker, uint8_t map_index, uint16_t report_id,
    const uint8_t *data, size_t length, solar_os_hid_keyboard_report_state_t *out)
{
    solar_os_hid_keyboard_report_tracker_t *state = solar_os_native_handle_acquire(tracker, SOLAR_OS_NATIVE_HANDLE_HID_REPORT);
    if (!state) return false;
    bool result = solar_os_hid_keyboard_report_update(state, map_index, report_id, data, length, out);
    solar_os_native_handle_release(tracker);
    return result;
}
static bool report_update_with_layout(solar_os_native_handle_t tracker, uint8_t map_index, uint16_t report_id,
    const uint8_t *data, size_t length, const solar_os_hid_keyboard_report_layout_t *layout,
    solar_os_hid_keyboard_report_state_t *out)
{
    solar_os_hid_keyboard_report_tracker_t *state = solar_os_native_handle_acquire(tracker, SOLAR_OS_NATIVE_HANDLE_HID_REPORT);
    if (!state) return false;
    bool result = solar_os_hid_keyboard_report_update_with_layout(state, map_index, report_id, data, length, layout, out);
    solar_os_native_handle_release(tracker);
    return result;
}
static const solar_os_native_hid_report_api_v1_t hid_report_api = {
    .abi_version = SOLAR_OS_NATIVE_HID_REPORT_ABI, .struct_size = sizeof(hid_report_api),
    .limits = report_limits, .create = report_create, .close = report_close,
    .reset = report_reset, .update = report_update, .update_with_layout = report_update_with_layout,
    .has_new_press = solar_os_hid_keyboard_report_has_new_press,
    .descriptor_map = solar_os_hid_report_descriptor_keyboard_map,
    .descriptor_layout = solar_os_hid_report_descriptor_keyboard_layout,
};
#endif
const void *solar_os_native_hid_report_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_hid_report_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
