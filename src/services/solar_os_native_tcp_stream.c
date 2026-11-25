#include "solar_os_native_tcp_stream.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_tcp_stream_abi.h"
#include "solar_os_native_support.h"
#if SOLAR_OS_PACKAGE_SERVICE_TCP_STREAM
#include "solar_os_tcp_stream.h"
#include "solar_os_port.h"
static int tcp_stream_limits(solar_os_native_tcp_stream_limits_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_tcp_stream_limits_v1_t){
        .maximum_endpoints = SOLAR_OS_TCP_STREAM_MAX, .name_bytes = SOLAR_OS_PORT_NAME_MAX};
    return SOLAR_OS_NATIVE_OK;
}
static int tcp_stream_listen(const char *name, uint32_t port)
{
    if (!port || port > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_tcp_stream_listen(name, (uint16_t)port));
}
static int tcp_stream_connect(const char *name, const char *host, uint32_t port)
{
    if (!port || port > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_tcp_stream_connect(name, host, (uint16_t)port));
}
static int tcp_stream_remove(const char *name)
{ return solar_os_native_result(solar_os_tcp_stream_remove(name)); }
static const solar_os_native_tcp_stream_api_v1_t tcp_stream_api = {
    .abi_version = SOLAR_OS_NATIVE_TCP_STREAM_ABI, .struct_size = sizeof(tcp_stream_api),
    .limits = tcp_stream_limits, .listen = tcp_stream_listen,
    .connect = tcp_stream_connect, .remove = tcp_stream_remove,
};
#endif
const void *solar_os_native_tcp_stream_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_tcp_stream_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
