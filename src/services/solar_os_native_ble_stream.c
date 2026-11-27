#include "solar_os_native_ble_stream.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_ble_stream_abi.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_BLE_STREAM
#include "solar_os_ble_stream.h"
#include "solar_os_port.h"
static int ble_stream_limits(solar_os_native_ble_stream_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_ble_stream_limits_v1_t){
        .maximum_endpoints = SOLAR_OS_BLE_STREAM_MAX, .name_bytes = SOLAR_OS_PORT_NAME_MAX};
    return SOLAR_OS_NATIVE_OK;
}
static int ble_stream_listen(const char *name)
{ return solar_os_native_result(solar_os_ble_stream_listen(name)); }
static int ble_stream_connect(const char *name, const uint8_t address[6], uint8_t type)
{ return solar_os_native_result(solar_os_ble_stream_connect(name, address, type)); }
static int ble_stream_remove(const char *name)
{ return solar_os_native_result(solar_os_ble_stream_remove(name)); }
static int ble_stream_status(const char *name, solar_os_ble_stream_info_t *info)
{
    if (info) *info = (solar_os_ble_stream_info_t){0};
    return solar_os_native_result(solar_os_ble_stream_status(name, info));
}
static const solar_os_native_ble_stream_api_v1_t ble_stream_api = {
    .abi_version = SOLAR_OS_NATIVE_BLE_STREAM_ABI, .struct_size = sizeof(ble_stream_api),
    .limits = ble_stream_limits, .listen = ble_stream_listen, .connect = ble_stream_connect,
    .remove = ble_stream_remove, .status = ble_stream_status,
};
#endif
const void *solar_os_native_ble_stream_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_ble_stream_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
