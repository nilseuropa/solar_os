#define main stream_baseline_checks
#include "native_stream_api_test.c"
#undef main
#include "solar_os_port.h"

static unsigned port_opened, port_closed, close_attempts;
static bool refuse_close;
static esp_err_t port_open(void *user)
{ assert(user == &port_opened); port_opened++; return ESP_OK; }
static esp_err_t port_close(void *user)
{
    assert(user == &port_opened); close_attempts++;
    if (refuse_close) return ESP_ERR_TIMEOUT;
    port_closed++; return ESP_OK;
}
static esp_err_t port_read(void *user, uint8_t *data, size_t capacity, uint32_t timeout, size_t *count)
{
    assert(user == &port_opened); (void)timeout;
    *count = capacity ? 1 : 0; if (capacity) data[0] = 42; return ESP_OK;
}
int main(void)
{
    assert(!stream_baseline_checks());
    const solar_os_native_stream_api_v1_t *api = solar_os_native_stream_get_service("stream", 1, sizeof(*api));
    solar_os_port_driver_t port = {.name="port0", .label="Retry test", .user=&port_opened,
        .capabilities=SOLAR_OS_PORT_CAP_READ, .open=port_open, .close=port_close, .read=port_read};
    assert(!solar_os_port_register(&port));
    solar_os_native_handle_t handle;
    assert(!api->open("port0", "native", NULL, &handle));
    assert(port_opened == 1 && allocations == 1 && solar_os_native_handle_count() == 1);
    refuse_close = true;
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(api->valid(handle) && allocations == 1 && solar_os_native_handle_count() == 1);
    solar_os_native_stream_info_v1_t info;
    assert(!api->find("port0", &info) && info.active_handles == 1 && !strcmp(info.owner, "native"));
    solar_os_port_info_t port_info;
    assert(!solar_os_port_get_info("port0", &port_info) && port_info.claimed);
    assert(!strcmp(port_info.owner, "native"));
    uint8_t byte; size_t count;
    assert(!api->read(handle, &byte, 1, 0, &count) && byte == 42 && count == 1);
    solar_os_port_handle_t other = SOLAR_OS_PORT_HANDLE_INIT;
    assert(solar_os_port_claim("port0", "other", &other) == ESP_ERR_INVALID_STATE);
    assert(solar_os_port_unregister("port0") == ESP_ERR_INVALID_STATE);
    refuse_close = false;
    assert(!api->close(handle) && !allocations && !solar_os_native_handle_count());
    assert(port_closed == 1 && close_attempts == 2 && !api->valid(handle));
    assert(!api->find("port0", &info) && !info.active_handles && !info.owner[0]);
    assert(!solar_os_port_get_info("port0", &port_info) && !port_info.claimed);
    assert(api->close(handle) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_stream_handle_t direct = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(!solar_os_stream_open_ex("port0", "direct", NULL, &direct));
    uintptr_t token = direct.private_data[1];
    refuse_close = true;
    assert(solar_os_stream_close_ex(&direct) == ESP_ERR_TIMEOUT);
    assert(solar_os_stream_handle_valid(&direct) && direct.private_data[1] == token);
    refuse_close = false;
    assert(!solar_os_stream_close_ex(&direct));
    assert(port_opened == 2 && port_closed == 2 && close_attempts == 4);
    assert(!solar_os_port_unregister("port0"));
    puts("Port stream close: refusal preserves native, stream and port claims; retry releases all three");
    return 0;
}
