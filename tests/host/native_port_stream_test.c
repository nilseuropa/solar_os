#define main stream_baseline_checks
#include "native_stream_api_test.c"
#undef main
#include "solar_os_port.h"
#include "solar_os_native_port_provider.h"
#include "solar_os_native_port_provider_abi.h"

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
static void check_port_group(void)
{
    solar_os_port_driver_t drivers[] = {
        {.name = "batch0", .capabilities = SOLAR_OS_PORT_CAP_READ, .read = port_read, .user = &port_opened},
        {.name = "batch1", .capabilities = SOLAR_OS_PORT_CAP_READ, .read = port_read, .user = &port_opened},
    };
    const char *names[] = {"batch0", "batch1"};
    solar_os_stream_driver_t occupied = {
        .info = {.id = "batch1", .provider = "test", .format = "f32", .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .sharing = SOLAR_OS_STREAM_SHARING_SHARED},
        .read_scalar_sample = sample,
    };
    assert(!solar_os_stream_register(&occupied));
    assert(solar_os_port_register_many(drivers, 2) == ESP_ERR_INVALID_STATE);
    solar_os_port_info_t info;
    solar_os_stream_info_t stream;
    assert(solar_os_port_get_info("batch0", &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_port_get_info("batch1", &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_stream_get_info("batch0", &stream) == ESP_ERR_NOT_FOUND);
    assert(!allocations);
    assert(!solar_os_stream_unregister("batch1"));
    assert(!solar_os_port_register_many(drivers, 2));
    assert(!allocations);
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    assert(!solar_os_port_claim("batch1", "probe", &handle));
    assert(solar_os_port_unregister_many(names, 2) == ESP_ERR_INVALID_STATE);
    assert(!solar_os_port_get_info("batch0", &info));
    assert(!solar_os_port_release(&handle));
    assert(!solar_os_port_unregister_many(names, 2));
    assert(solar_os_stream_get_info("batch0", &stream) == ESP_ERR_NOT_FOUND);
    assert(solar_os_stream_get_info("batch1", &stream) == ESP_ERR_NOT_FOUND);
}

static int native_port_read(void *user, uint8_t *data, size_t capacity,
                            uint32_t timeout_ms, size_t *count)
{
    assert(provider_leases == 1 && user == &port_opened && timeout_ms == 0);
    *count = capacity ? 1 : 0;
    if (capacity) data[0] = 71;
    return SOLAR_OS_NATIVE_OK;
}

static int native_serial_get(void *user, solar_os_native_serial_config_v1_t *config)
{
    assert(provider_leases == 1 && user == &port_opened);
    *config = (solar_os_native_serial_config_v1_t){.baud_rate = 9600, .data_bits = 8, .dtr = 1};
    return SOLAR_OS_NATIVE_OK;
}

static void check_native_port_group(void)
{
    const solar_os_native_port_provider_api_v1_t *abi = solar_os_native_port_provider_get_service(
        SOLAR_OS_NATIVE_PORT_PROVIDER_SERVICE, SOLAR_OS_NATIVE_PORT_PROVIDER_ABI, sizeof(*abi));
    assert(abi && !solar_os_native_port_provider_get_service(
        SOLAR_OS_NATIVE_PORT_PROVIDER_SERVICE, SOLAR_OS_NATIVE_PORT_PROVIDER_ABI + 1, sizeof(*abi)));
    char name[] = "native0";
    const solar_os_native_port_provider_endpoint_v1_t endpoints[] = {
        {.name = name, .capabilities = SOLAR_OS_NATIVE_PORT_READ | SOLAR_OS_NATIVE_PORT_SERIAL,
            .callbacks = {.read = native_port_read, .serial_get = native_serial_get, .user = &port_opened}},
        {.name = "native1", .capabilities = SOLAR_OS_NATIVE_PORT_READ,
            .callbacks = {.read = native_port_read, .user = &port_opened}},
    };
    solar_os_native_handle_t registration;
    assert(abi->register_many(0, endpoints, 2, &registration) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!registration && !provider_leases && !allocations);
    assert(!abi->register_many(6, endpoints, 2, &registration));
    assert(registration && allocations == 1 && provider_leases == 1);
    name[0] = 'X';

    solar_os_port_handle_t port = SOLAR_OS_PORT_HANDLE_INIT;
    assert(!solar_os_port_claim("native0", "serial", &port));
    assert(abi->close(registration) == SOLAR_OS_NATIVE_ERROR_BUSY);
    uint8_t byte = 0;
    size_t received;
    assert(!solar_os_port_read(&port, &byte, 1, 0, &received) && received == 1 && byte == 71);
    solar_os_port_serial_config_t config;
    assert(!solar_os_port_serial_get(&port, &config));
    assert(config.baud_rate == 9600 && config.data_bits == 8 && config.dtr);
    assert(!solar_os_port_release(&port));

    /* Independent replacements with the same names survive owner cleanup. */
    assert(!solar_os_port_unregister("native1"));
    const solar_os_port_driver_t replacement = {
        .name = "native1", .capabilities = SOLAR_OS_PORT_CAP_READ, .read = port_read, .user = &port_opened,
    };
    assert(!solar_os_port_register(&replacement));
    assert(!solar_os_stream_unregister("native0"));
    const solar_os_stream_driver_t independent = {
        .info = {.id = "native0", .provider = "test", .format = "f32", .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE, .sharing = SOLAR_OS_STREAM_SHARING_SHARED},
        .read_scalar_sample = sample,
    };
    assert(!solar_os_stream_register(&independent));
    assert(!abi->close(registration));
    assert(!allocations && !provider_leases && !solar_os_native_handle_count());
    solar_os_port_info_t info;
    solar_os_stream_info_t stream;
    assert(solar_os_port_get_info("native0", &info) == ESP_ERR_NOT_FOUND);
    assert(!solar_os_port_get_info("native1", &info));
    assert(!solar_os_stream_get_info("native0", &stream));
    assert(stream.type == SOLAR_OS_STREAM_TYPE_SCALAR);
    assert(!solar_os_port_unregister("native1"));
    assert(!solar_os_stream_unregister("native0"));
}
int main(void)
{
    assert(!stream_baseline_checks());
    check_port_group();
    check_native_port_group();
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
