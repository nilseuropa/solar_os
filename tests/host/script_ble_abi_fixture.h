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
static const void *script_ble_lookup(const char *name, uint32_t abi, uint32_t minimum)
{
    ++script_ble_lookups;
    if (!script_ble_available) return NULL;
    const void *api = solar_os_native_ble_get_service(name, abi, minimum);
    if (api) return api;
    api = solar_os_native_ble_keyboard_get_service(name, abi, minimum);
    if (!api) api = solar_os_native_ble_stream_get_service(name, abi, minimum);
    return api ? api : solar_os_native_tcp_stream_get_service(name, abi, minimum);
}
static const solar_os_native_host_api_v1_t script_ble_host = {
    .abi_version = 1, .struct_size = sizeof(script_ble_host), .get_service = script_ble_lookup,
};
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void) { return &script_ble_host; }
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
