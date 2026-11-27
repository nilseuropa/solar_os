#include "solar_os_native_tcp_stream.h"
#include "solar_os_native_tcp_stream_abi.h"
#include "solar_os_tcp_stream.h"
#include "solar_os_native_support.h"
static const solar_os_native_tcp_stream_api_v1_t *api;
static esp_err_t abi_listen(const char *name, uint16_t port)
{ return solar_os_native_callback_result(api->listen(name, port)); }
static esp_err_t abi_connect(const char *name, const char *host, uint16_t port)
{ return solar_os_native_callback_result(api->connect(name, host, port)); }
static esp_err_t abi_remove(const char *name)
{ return solar_os_native_callback_result(api->remove(name)); }
#define solar_os_tcp_stream_listen abi_listen
#define solar_os_tcp_stream_connect abi_connect
#define solar_os_tcp_stream_remove abi_remove
#define main tcp_registry_checks
#include "tcp_stream_test.c"
#undef main
#undef solar_os_tcp_stream_listen
#undef solar_os_tcp_stream_connect
#undef solar_os_tcp_stream_remove
int main(void)
{
    api = solar_os_native_tcp_stream_get_service("stream.tcp", 1, sizeof(*api));
    assert(api && api->abi_version == 1 && api->struct_size == sizeof(*api));
    assert(!solar_os_native_tcp_stream_get_service("stream.tcp", 2, 8));
    assert(!solar_os_native_tcp_stream_get_service("stream.tcp", 1, sizeof(*api) + 1));
    assert(!solar_os_native_tcp_stream_get_service("stream.ble", 1, 8));
    solar_os_native_tcp_stream_limits_v1_t limits;
    assert(api->limits(NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->limits(&limits) && limits.maximum_endpoints == SOLAR_OS_TCP_STREAM_MAX && limits.name_bytes > 1);
    assert(api->listen("bad", 65536) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->connect("bad", "localhost", UINT32_MAX) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->listen(NULL, 1234) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->connect("bad", NULL, 1234) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!tcp_registry_checks());
    puts("TCP stream ABI: real loopback transport, exclusive ownership, binary I/O and normalized configuration passed");
    return 0;
}
