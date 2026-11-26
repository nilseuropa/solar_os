#pragma once
#include <stdlib.h>
#include "solar_os_native_client.h"
#include "solar_os_native_ble.h"
#include "solar_os_native_ble_keyboard.h"
#include "solar_os_native_ble_stream.h"
#include "solar_os_native_ble_stream_abi.h"
#include "solar_os_native_tcp_stream.h"
#include "ble_keyboard_abi_backend.h"
#include "solar_os_memory.h"

static bool script_ble_available = true;
static unsigned script_ble_lookups, script_ble_adapter_allocations;
#define SCRIPT_QUERY_WRAPPER(id, provider) \
    static const void *script_query_##id(const char *name, uint32_t abi, uint32_t minimum) \
    { \
        ++script_ble_lookups; \
        return script_ble_available ? solar_os_native_##provider##_get_service(name, abi, minimum) : NULL; \
    }
SCRIPT_QUERY_WRAPPER(bluetooth, ble)
SCRIPT_QUERY_WRAPPER(ble_keyboard, ble_keyboard)
SCRIPT_QUERY_WRAPPER(ble_stream, ble_stream)
SCRIPT_QUERY_WRAPPER(tcp_stream, tcp_stream)
#undef SCRIPT_QUERY_WRAPPER
#define SOLAR_OS_SCRIPT_SERVICE_QUERY(id) script_query_##id

void *solar_os_memory_calloc(size_t count, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    (void)kind; (void)tag;
    ++script_ble_adapter_allocations;
    return calloc(count, size);
}
void solar_os_memory_free(void *value) { free(value); }

/* These VM cases manage definitions, while the separate real registry corpus
 * exercises claims and transport I/O. Opening a channel here is unexpected. */
uint16_t solar_os_ble_backend_coc_mtu(void) { return 246; }
esp_err_t solar_os_ble_backend_coc_open(solar_os_ble_session_t owner,uint16_t connection,
    bool listener,uint32_t *out)
{ (void)owner;(void)connection;(void)listener;(void)out;assert(false);return ESP_ERR_NOT_SUPPORTED; }
void solar_os_ble_backend_coc_close(uint32_t token) { assert(!token); }
esp_err_t solar_os_ble_backend_coc_read(uint32_t token,uint8_t *data,size_t capacity,size_t *out)
{ (void)token;(void)data;(void)capacity;(void)out;assert(false);return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_ble_backend_coc_write(uint32_t token,const uint8_t *data,size_t size,size_t *out)
{ (void)token;(void)data;(void)size;(void)out;assert(false);return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_ble_backend_coc_status(uint32_t token,solar_os_ble_coc_info_t *out)
{ (void)token;(void)out;assert(false);return ESP_ERR_NOT_SUPPORTED; }
uint64_t solar_os_time_uptime_ms(void) { return xTaskGetTickCount(); }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *out) { *out=0;return ESP_OK; }
esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{ (void)owner;assert(false);return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_wifi_latency_release(const char *owner)
{ (void)owner;assert(false);return ESP_ERR_NOT_SUPPORTED; }
