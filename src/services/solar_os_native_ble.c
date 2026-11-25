#include "solar_os_native_ble.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_memory.h"
#include "solar_os_native_ble_abi.h"
#include "solar_os_native_bluetooth_abi.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE
#include "solar_os_ble.h"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_BLE
_Static_assert(SOLAR_OS_NATIVE_BLE_VALUE_MAX == SOLAR_OS_BLE_GATT_VALUE_MAX,
               "native BLE value ABI must match the service");
_Static_assert(SOLAR_OS_NATIVE_BLE_NAME_MAX == SOLAR_OS_BLE_NAME_MAX,
               "native BLE name ABI must match the service");
_Static_assert(SOLAR_OS_NATIVE_BLE_UUID_MAX == SOLAR_OS_BLE_GATT_UUID_MAX,
               "native BLE UUID ABI must match the service");

static int native_ble_result(esp_err_t result)
{
    switch (result) {
    case ESP_OK: return SOLAR_OS_NATIVE_BLE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_BLE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_BLE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_BLE_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_SIZE;
    case SOLAR_OS_BLE_ERR_CANCELLED: return SOLAR_OS_NATIVE_BLE_ERROR_CANCELLED;
    case SOLAR_OS_BLE_ERR_CAPACITY: return SOLAR_OS_NATIVE_BLE_ERROR_CAPACITY;
    default: return SOLAR_OS_NATIVE_BLE_ERROR_FAILED;
    }
}

static int native_ble_init(void)
{
    return native_ble_result(solar_os_ble_init());
}

static int native_ble_scan(solar_os_native_ble_scan_result_v1_t *results,
                           size_t max_results, size_t *found)
{
    if (found == NULL || max_results > SOLAR_OS_BLE_SCAN_MAX_RESULTS ||
        (max_results != 0U && results == NULL)) {
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    }
    solar_os_ble_scan_result_t *temporary = max_results != 0U ?
        solar_os_memory_calloc(max_results, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.scan") : NULL;
    if (max_results != 0U && temporary == NULL) {
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    }
    const esp_err_t ret = solar_os_ble_scan(temporary, max_results, found);
    if (ret == ESP_OK) {
        const size_t count = *found < max_results ? *found : max_results;
        for (size_t i = 0; i < count; ++i) {
            memcpy(results[i].bda, temporary[i].bda, sizeof(results[i].bda));
            results[i].addr_type = temporary[i].addr_type;
            results[i].rssi = temporary[i].rssi;
            results[i].appearance = temporary[i].appearance;
            results[i].hid_service = temporary[i].hid_service;
            results[i].keyboard_like = temporary[i].keyboard_like;
            results[i].remembered = temporary[i].remembered;
            results[i].connected = temporary[i].connected;
            memcpy(results[i].name, temporary[i].name, sizeof(results[i].name));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_session_create(const char *owner,
                                     solar_os_native_ble_session_t *session)
{
    return native_ble_result(solar_os_ble_session_create(owner, session));
}

static int native_ble_session_cancel(solar_os_native_ble_session_t session)
{
    return native_ble_result(solar_os_ble_session_cancel(session));
}

static int native_ble_session_close(solar_os_native_ble_session_t session)
{
    return native_ble_result(solar_os_ble_session_close(session));
}

static int native_ble_peer_connect(solar_os_native_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms,
    solar_os_native_ble_peer_t *peer)
{
    return native_ble_result(solar_os_ble_peer_connect(session, bda, addr_type,
                                                        timeout_ms, peer));
}

static int native_ble_peer_disconnect(solar_os_native_ble_session_t session,
                                      solar_os_native_ble_peer_t peer)
{
    return native_ble_result(solar_os_ble_peer_disconnect(session, peer));
}

static int native_ble_peer_pair(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint32_t passkey, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_pair(session, peer, passkey,
                                                     timeout_ms));
}

static int native_ble_peer_info(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                solar_os_native_ble_peer_info_v1_t *info)
{
    if (info == NULL) return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_session_info_t source = {0};
    const esp_err_t ret = solar_os_ble_peer_get_info(session, peer, &source);
    if (ret != ESP_OK) return native_ble_result(ret);
    memset(info, 0, sizeof(*info));
    memcpy(info->owner, source.owner, sizeof(info->owner));
    info->busy = source.busy;
    info->retiring = source.retiring;
    info->event_capacity = source.event_capacity;
    info->event_count = source.event_count;
    info->events_dropped = source.events_dropped;
    info->gatt.connected = source.gatt.connected;
    info->gatt.encrypted = source.gatt.encrypted;
    info->gatt.bonded = source.gatt.bonded;
    memcpy(info->gatt.bda, source.gatt.bda, sizeof(info->gatt.bda));
    info->gatt.addr_type = source.gatt.addr_type;
    info->gatt.conn_id = source.gatt.conn_id;
    info->gatt.mtu = source.gatt.mtu;
    info->gatt.service_count = source.gatt.service_count;
    memcpy(info->gatt.status, source.gatt.status, sizeof(info->gatt.status));
    return SOLAR_OS_NATIVE_BLE_OK;
}

static int native_ble_peer_services(solar_os_native_ble_session_t session,
                                    solar_os_native_ble_peer_t peer,
                                    solar_os_native_ble_service_v1_t *services,
                                    size_t max_services, size_t *count)
{
    if (count == NULL || max_services > SOLAR_OS_BLE_GATT_MAX_SERVICES ||
        (max_services != 0U && services == NULL))
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_gatt_service_t *temporary = max_services != 0U ?
        solar_os_memory_calloc(max_services, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.svcs") : NULL;
    if (max_services != 0U && temporary == NULL)
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    const esp_err_t ret = solar_os_ble_peer_services(session, peer, temporary,
                                                      max_services, count);
    if (ret == ESP_OK) {
        const size_t copied = *count < max_services ? *count : max_services;
        for (size_t i = 0; i < copied; ++i) {
            services[i].start_handle = temporary[i].start_handle;
            services[i].end_handle = temporary[i].end_handle;
            services[i].primary = temporary[i].primary;
            memcpy(services[i].uuid, temporary[i].uuid, sizeof(services[i].uuid));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_peer_characteristics(
    solar_os_native_ble_session_t session, solar_os_native_ble_peer_t peer,
    size_t service_index, solar_os_native_ble_characteristic_v1_t *chars,
    size_t max_chars, size_t *count)
{
    if (count == NULL || max_chars > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ||
        (max_chars != 0U && chars == NULL))
        return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_gatt_characteristic_t *temporary = max_chars != 0U ?
        solar_os_memory_calloc(max_chars, sizeof(*temporary),
                               SOLAR_OS_MEMORY_TRANSIENT, "native.ble.chars") : NULL;
    if (max_chars != 0U && temporary == NULL)
        return SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY;
    const esp_err_t ret = solar_os_ble_peer_characteristics(session, peer,
        service_index, temporary, max_chars, count);
    if (ret == ESP_OK) {
        const size_t copied = *count < max_chars ? *count : max_chars;
        for (size_t i = 0; i < copied; ++i) {
            chars[i].handle = temporary[i].handle;
            chars[i].properties = temporary[i].properties;
            memcpy(chars[i].uuid, temporary[i].uuid, sizeof(chars[i].uuid));
        }
    }
    solar_os_memory_free(temporary);
    return native_ble_result(ret);
}

static int native_ble_peer_configure_queue(
    solar_os_native_ble_session_t session, solar_os_native_ble_peer_t peer,
    size_t capacity)
{
    return native_ble_result(solar_os_ble_peer_configure_queue(session, peer,
                                                                capacity));
}

static int native_ble_peer_subscribe(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, uint8_t mode,
    uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_subscribe(session, peer, handle,
                                                          mode, timeout_ms));
}

static int native_ble_peer_poll(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                solar_os_native_ble_notification_v1_t *event)
{
    if (event == NULL) return SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT;
    solar_os_ble_notification_t source = {0};
    const esp_err_t ret = solar_os_ble_peer_poll(session, peer, &source);
    if (ret != ESP_OK) return native_ble_result(ret);
    event->handle = source.handle;
    event->indication = source.indication;
    event->value_len = source.value_len;
    memcpy(event->value, source.value, source.value_len);
    return SOLAR_OS_NATIVE_BLE_OK;
}

static int native_ble_peer_read(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, uint8_t *value,
    size_t max_len, size_t *value_len, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_read(session, peer, handle,
        value, max_len, value_len, timeout_ms));
}

static int native_ble_peer_write(solar_os_native_ble_session_t session,
    solar_os_native_ble_peer_t peer, uint16_t handle, const uint8_t *value,
    size_t value_len, bool with_response, uint32_t timeout_ms)
{
    return native_ble_result(solar_os_ble_peer_write(session, peer, handle,
        value, value_len, with_response, timeout_ms));
}

static const solar_os_native_ble_client_api_v1_t native_ble_client_v1 = {
    .abi_version = SOLAR_OS_NATIVE_BLE_CLIENT_ABI,
    .struct_size = sizeof(solar_os_native_ble_client_api_v1_t),
    .max_value_size = SOLAR_OS_BLE_GATT_VALUE_MAX,
    .init = native_ble_init,
    .scan = native_ble_scan,
    .session_create = native_ble_session_create,
    .session_cancel = native_ble_session_cancel,
    .session_close = native_ble_session_close,
    .peer_capacity = solar_os_ble_peer_capacity,
    .peer_connect = native_ble_peer_connect,
    .peer_disconnect = native_ble_peer_disconnect,
    .peer_pair = native_ble_peer_pair,
    .peer_get_info = native_ble_peer_info,
    .peer_services = native_ble_peer_services,
    .peer_characteristics = native_ble_peer_characteristics,
    .peer_configure_queue = native_ble_peer_configure_queue,
    .peer_subscribe = native_ble_peer_subscribe,
    .peer_poll = native_ble_peer_poll,
    .peer_read = native_ble_peer_read,
    .peer_write = native_ble_peer_write,
};
#endif


#if SOLAR_OS_PACKAGE_SERVICE_BLE
static int bluetooth_result(esp_err_t error)
{
    if (error == SOLAR_OS_BLE_ERR_CANCELLED) return SOLAR_OS_NATIVE_ERROR_CANCELLED;
    if (error == SOLAR_OS_BLE_ERR_CAPACITY) return SOLAR_OS_NATIVE_ERROR_NO_SPACE;
    return solar_os_native_result(error);
}
static int bluetooth_limits(solar_os_native_bluetooth_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_bluetooth_limits_v1_t){
        .scan_results = SOLAR_OS_BLE_SCAN_MAX_RESULTS, .gatt_services = SOLAR_OS_BLE_GATT_MAX_SERVICES,
        .gatt_characteristics = SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, .value_bytes = SOLAR_OS_BLE_GATT_VALUE_MAX,
        .owner_bytes = SOLAR_OS_BLE_OWNER_MAX, .hid_hosts = SOLAR_OS_BLE_HID_HOST_MAX,
    };
    return SOLAR_OS_NATIVE_OK;
}
static int bluetooth_hid_device_start(solar_os_ble_session_t session, const char *name)
{ return bluetooth_result(solar_os_ble_hid_device_start(session, name)); }
static int bluetooth_hid_device_start_managed(solar_os_ble_session_t session, const char *name)
{ return bluetooth_result(solar_os_ble_hid_device_start_managed(session, name)); }
static int bluetooth_hid_device_hosts(solar_os_ble_session_t session, solar_os_ble_hid_host_t *hosts, size_t capacity, size_t *count)
{ return bluetooth_result(solar_os_ble_hid_device_hosts(session, hosts, capacity, count)); }
static int bluetooth_hid_device_connect(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type)
{ return bluetooth_result(solar_os_ble_hid_device_connect(session, bda, addr_type)); }
static int bluetooth_hid_device_forget(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type)
{ return bluetooth_result(solar_os_ble_hid_device_forget(session, bda, addr_type)); }
static int bluetooth_hid_device_disconnect(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_hid_device_disconnect(session)); }
static int bluetooth_hid_device_pair(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_hid_device_pair(session)); }
static int bluetooth_hid_device_stop(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_hid_device_stop(session)); }
static int bluetooth_hid_device_status(solar_os_ble_session_t session, solar_os_ble_hid_info_t *info)
{ return bluetooth_result(solar_os_ble_hid_device_status(session, info)); }
static int bluetooth_hid_device_poll(solar_os_ble_session_t session, solar_os_ble_hid_device_event_t *event)
{ return bluetooth_result(solar_os_ble_hid_device_poll(session, event)); }
static int bluetooth_hid_keyboard_press(solar_os_ble_session_t session, const uint16_t *keys, size_t key_count)
{ return bluetooth_result(solar_os_ble_hid_keyboard_press(session, keys, key_count)); }
static int bluetooth_hid_keyboard_release(solar_os_ble_session_t session, const uint16_t *keys, size_t key_count)
{ return bluetooth_result(solar_os_ble_hid_keyboard_release(session, keys, key_count)); }
static int bluetooth_hid_keyboard_release_all(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_hid_keyboard_release_all(session)); }
static int bluetooth_hid_mouse_move(solar_os_ble_session_t session, int32_t x, int32_t y)
{ return bluetooth_result(solar_os_ble_hid_mouse_move(session, x, y)); }
static int bluetooth_hid_mouse_button(solar_os_ble_session_t session, uint8_t button, bool pressed)
{ return bluetooth_result(solar_os_ble_hid_mouse_button(session, button, pressed)); }
static int bluetooth_hid_gamepad_axis(solar_os_ble_session_t session, int axis, int16_t value)
{ return bluetooth_result(solar_os_ble_hid_gamepad_axis(session, axis, value)); }
static int bluetooth_hid_gamepad_button(solar_os_ble_session_t session, uint8_t button, bool pressed)
{ return bluetooth_result(solar_os_ble_hid_gamepad_button(session, button, pressed)); }
static int bluetooth_hid_gamepad_hat(solar_os_ble_session_t session, uint8_t hat)
{ return bluetooth_result(solar_os_ble_hid_gamepad_hat(session, hat)); }
static int bluetooth_hid_gamepad_send(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_hid_gamepad_send(session)); }
static int bluetooth_server_request(solar_os_ble_session_t session, solar_os_ble_server_request_t *request)
{ return bluetooth_result(solar_os_ble_server_request(session, request)); }
static int bluetooth_session_create(const char *owner, solar_os_ble_session_t *session)
{ return bluetooth_result(solar_os_ble_session_create(owner, session)); }
static int bluetooth_session_close(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_session_close(session)); }
static int bluetooth_session_cancel(solar_os_ble_session_t session)
{ return bluetooth_result(solar_os_ble_session_cancel(session)); }
static int bluetooth_session_get_info(solar_os_ble_session_t session, solar_os_ble_session_info_t *info)
{ return bluetooth_result(solar_os_ble_session_get_info(session, info)); }
static int bluetooth_session_connect(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_session_connect(session, bda, addr_type, timeout_ms)); }
static int bluetooth_session_pair(solar_os_ble_session_t session, uint32_t passkey, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_session_pair(session, passkey, timeout_ms)); }
static int bluetooth_session_services(solar_os_ble_session_t session, solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{ return bluetooth_result(solar_os_ble_session_services(session, services, max_services, count)); }
static int bluetooth_session_characteristics(solar_os_ble_session_t session, size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics, size_t max_characteristics, size_t *count)
{ return bluetooth_result(solar_os_ble_session_characteristics(session, service_index, characteristics, max_characteristics, count)); }
static int bluetooth_session_read(solar_os_ble_session_t session, uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_session_read(session, handle, value, max_len, value_len, timeout_ms)); }
static int bluetooth_session_write(solar_os_ble_session_t session, uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_session_write(session, handle, value, value_len, with_response, timeout_ms)); }
static int bluetooth_peer_configure_queue(solar_os_ble_session_t session, solar_os_ble_peer_t peer, size_t capacity)
{ return bluetooth_result(solar_os_ble_peer_configure_queue(session, peer, capacity)); }
static int bluetooth_peer_poll(solar_os_ble_session_t session, solar_os_ble_peer_t peer, solar_os_ble_notification_t *event)
{ return bluetooth_result(solar_os_ble_peer_poll(session, peer, event)); }
static int bluetooth_peer_subscribe(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, uint8_t mode, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_peer_subscribe(session, peer, handle, mode, timeout_ms)); }
static int bluetooth_peer_connect(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms, solar_os_ble_peer_t *peer)
{ return bluetooth_result(solar_os_ble_peer_connect(session, bda, addr_type, timeout_ms, peer)); }
static int bluetooth_peer_disconnect(solar_os_ble_session_t session, solar_os_ble_peer_t peer)
{ return bluetooth_result(solar_os_ble_peer_disconnect(session, peer)); }
static int bluetooth_peer_pair(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint32_t passkey, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_peer_pair(session, peer, passkey, timeout_ms)); }
static int bluetooth_peer_get_info(solar_os_ble_session_t session, solar_os_ble_peer_t peer, solar_os_ble_session_info_t *info)
{ return bluetooth_result(solar_os_ble_peer_get_info(session, peer, info)); }
static int bluetooth_peer_services(solar_os_ble_session_t session, solar_os_ble_peer_t peer, solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{ return bluetooth_result(solar_os_ble_peer_services(session, peer, services, max_services, count)); }
static int bluetooth_peer_characteristics(solar_os_ble_session_t session, solar_os_ble_peer_t peer, size_t service_index, solar_os_ble_gatt_characteristic_t *chars, size_t max_chars, size_t *count)
{ return bluetooth_result(solar_os_ble_peer_characteristics(session, peer, service_index, chars, max_chars, count)); }
static int bluetooth_peer_read(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_peer_read(session, peer, handle, value, max_len, value_len, timeout_ms)); }
static int bluetooth_peer_write(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_peer_write(session, peer, handle, value, value_len, with_response, timeout_ms)); }
static int bluetooth_init(void)
{ return bluetooth_result(solar_os_ble_init()); }
static int bluetooth_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found)
{ return bluetooth_result(solar_os_ble_scan(results, max_results, found)); }
static int bluetooth_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_gatt_connect(bda, addr_type, timeout_ms)); }
static int bluetooth_gatt_pair(uint32_t passkey, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_gatt_pair(passkey, timeout_ms)); }
static int bluetooth_gatt_disconnect(void)
{ return bluetooth_result(solar_os_ble_gatt_disconnect()); }
static int bluetooth_gatt_services(solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{ return bluetooth_result(solar_os_ble_gatt_services(services, max_services, count)); }
static int bluetooth_gatt_characteristics(size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics, size_t max_characteristics, size_t *count)
{ return bluetooth_result(solar_os_ble_gatt_characteristics(service_index, characteristics, max_characteristics, count)); }
static int bluetooth_gatt_read(uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_gatt_read(handle, value, max_len, value_len, timeout_ms)); }
static int bluetooth_gatt_write(uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{ return bluetooth_result(solar_os_ble_gatt_write(handle, value, value_len, with_response, timeout_ms)); }
static int bluetooth_session_connect_with_options(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_session_connect_with_options(session, bda, addr_type, timeout_ms, options)); }
static int bluetooth_session_pair_with_options(solar_os_ble_session_t session, uint32_t passkey, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_session_pair_with_options(session, passkey, timeout_ms, options)); }
static int bluetooth_session_read_with_options(solar_os_ble_session_t session, uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_session_read_with_options(session, handle, value, max_len, value_len, timeout_ms, options)); }
static int bluetooth_session_write_with_options(solar_os_ble_session_t session, uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_session_write_with_options(session, handle, value, value_len, with_response, timeout_ms, options)); }
static int bluetooth_peer_connect_with_options(solar_os_ble_session_t session, const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms, solar_os_ble_peer_t *peer, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_peer_connect_with_options(session, bda, addr_type, timeout_ms, peer, options)); }
static int bluetooth_peer_pair_with_options(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint32_t passkey, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_peer_pair_with_options(session, peer, passkey, timeout_ms, options)); }
static int bluetooth_peer_read_with_options(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_peer_read_with_options(session, peer, handle, value, max_len, value_len, timeout_ms, options)); }
static int bluetooth_peer_write_with_options(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_peer_write_with_options(session, peer, handle, value, value_len, with_response, timeout_ms, options)); }
static int bluetooth_peer_subscribe_with_options(solar_os_ble_session_t session, solar_os_ble_peer_t peer, uint16_t handle, uint8_t mode, uint32_t timeout_ms, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_peer_subscribe_with_options(session, peer, handle, mode, timeout_ms, options)); }
static int bluetooth_server_request_with_options(solar_os_ble_session_t session, solar_os_ble_server_request_t *request, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_server_request_with_options(session, request, options)); }
static int bluetooth_hid_request_with_options(solar_os_ble_session_t session, solar_os_ble_hid_request_t *request, const solar_os_ble_call_options_v1_t *options)
{ return bluetooth_result(solar_os_ble_hid_request_with_options(session, request, options)); }
static const solar_os_native_bluetooth_api_v1_t bluetooth_api = {
    .abi_version = SOLAR_OS_NATIVE_BLUETOOTH_ABI, .struct_size = sizeof(bluetooth_api),
    .session_connect_with_options = bluetooth_session_connect_with_options,
    .session_pair_with_options = bluetooth_session_pair_with_options,
    .session_read_with_options = bluetooth_session_read_with_options,
    .session_write_with_options = bluetooth_session_write_with_options,
    .peer_connect_with_options = bluetooth_peer_connect_with_options,
    .peer_pair_with_options = bluetooth_peer_pair_with_options,
    .peer_read_with_options = bluetooth_peer_read_with_options,
    .peer_write_with_options = bluetooth_peer_write_with_options,
    .peer_subscribe_with_options = bluetooth_peer_subscribe_with_options,
    .server_request_with_options = bluetooth_server_request_with_options,
    .hid_request_with_options = bluetooth_hid_request_with_options,
    .limits = bluetooth_limits,
    .hid_device_start = bluetooth_hid_device_start,
    .hid_device_start_managed = bluetooth_hid_device_start_managed,
    .hid_device_hosts = bluetooth_hid_device_hosts,
    .hid_device_connect = bluetooth_hid_device_connect,
    .hid_device_forget = bluetooth_hid_device_forget,
    .hid_device_disconnect = bluetooth_hid_device_disconnect,
    .hid_device_pair = bluetooth_hid_device_pair,
    .hid_device_stop = bluetooth_hid_device_stop,
    .hid_device_status = bluetooth_hid_device_status,
    .hid_device_poll = bluetooth_hid_device_poll,
    .hid_keyboard_press = bluetooth_hid_keyboard_press,
    .hid_keyboard_release = bluetooth_hid_keyboard_release,
    .hid_keyboard_release_all = bluetooth_hid_keyboard_release_all,
    .hid_mouse_move = bluetooth_hid_mouse_move,
    .hid_mouse_button = bluetooth_hid_mouse_button,
    .hid_gamepad_axis = bluetooth_hid_gamepad_axis,
    .hid_gamepad_button = bluetooth_hid_gamepad_button,
    .hid_gamepad_hat = bluetooth_hid_gamepad_hat,
    .hid_gamepad_send = bluetooth_hid_gamepad_send,
    .server_request = bluetooth_server_request,
    .session_create = bluetooth_session_create,
    .parse_address = solar_os_ble_parse_address,
    .session_close = bluetooth_session_close,
    .session_cancel = bluetooth_session_cancel,
    .session_get_info = bluetooth_session_get_info,
    .session_connect = bluetooth_session_connect,
    .session_pair = bluetooth_session_pair,
    .session_services = bluetooth_session_services,
    .session_characteristics = bluetooth_session_characteristics,
    .session_read = bluetooth_session_read,
    .session_write = bluetooth_session_write,
    .peer_capacity = solar_os_ble_peer_capacity,
    .peer_configure_queue = bluetooth_peer_configure_queue,
    .peer_poll = bluetooth_peer_poll,
    .peer_subscribe = bluetooth_peer_subscribe,
    .peer_connect = bluetooth_peer_connect,
    .peer_disconnect = bluetooth_peer_disconnect,
    .peer_pair = bluetooth_peer_pair,
    .peer_get_info = bluetooth_peer_get_info,
    .peer_services = bluetooth_peer_services,
    .peer_characteristics = bluetooth_peer_characteristics,
    .peer_read = bluetooth_peer_read,
    .peer_write = bluetooth_peer_write,
    .init = bluetooth_init,
    .scan = bluetooth_scan,
    .gatt_connect = bluetooth_gatt_connect,
    .gatt_pair = bluetooth_gatt_pair,
    .gatt_disconnect = bluetooth_gatt_disconnect,
    .gatt_get_status = solar_os_ble_gatt_get_status,
    .gatt_services = bluetooth_gatt_services,
    .gatt_characteristics = bluetooth_gatt_characteristics,
    .gatt_read = bluetooth_gatt_read,
    .gatt_write = bluetooth_gatt_write,
};
#endif

const void *solar_os_native_ble_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ble_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
