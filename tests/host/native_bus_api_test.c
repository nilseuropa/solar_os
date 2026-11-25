#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_bus.h"
#include "solar_os_native_handles.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"
static const solar_os_native_bus_api_v1_t *api;
static solar_os_bus_info_t infos[6];
static char owners[6][24];
static unsigned allocations, transfers, acquired, released, cancelled;
static bool fail_release, fail_cancel, fail_finish;
static solar_os_native_handle_t current;
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
static unsigned by_name(const char *name) { assert(strlen(name) == 2 && name[0] == 'b' && name[1] >= '0' && name[1] <= '5'); return name[1] - '0'; }
bool solar_os_bus_runtime_protocol_available(solar_os_bus_protocol_t p) { return p != SOLAR_OS_BUS_PROTOCOL_PS2; }
size_t solar_os_bus_runtime_endpoint_count(solar_os_bus_protocol_t p) { return p == SOLAR_OS_BUS_PROTOCOL_I2C ? 2 : 1; }
bool solar_os_bus_runtime_endpoint_get(solar_os_bus_protocol_t p, size_t index, int *out)
{ if (index) return false; *out = p == SOLAR_OS_BUS_PROTOCOL_SPI ? 3 : 0; return true; }
esp_err_t solar_os_bus_register(const solar_os_bus_definition_t *in)
{
    unsigned index = by_name(in->name); assert((unsigned)in->protocol == index);
    infos[index] = (solar_os_bus_info_t){.active = true, .attached = true, .id = 99, .protocol = in->protocol,
        .origin = in->origin, .sharing = in->sharing, .config = in->config}; strcpy(infos[index].name, in->name); return ESP_OK;
}
esp_err_t solar_os_bus_unregister(const char *name) { infos[by_name(name)].active = false; return ESP_OK; }
esp_err_t solar_os_bus_attach(const char *name) { infos[by_name(name)].attached = true; return ESP_OK; }
esp_err_t solar_os_bus_detach(const char *name) { infos[by_name(name)].attached = false; return ESP_OK; }
size_t solar_os_bus_count(void) { return 6; }
size_t solar_os_bus_count_protocol(solar_os_bus_protocol_t p) { (void)p; return 1; }
bool solar_os_bus_get(size_t index, solar_os_bus_info_t *out) { if (index >= 6) return false; *out = infos[index]; return true; }
bool solar_os_bus_get_protocol(solar_os_bus_protocol_t p, size_t index, solar_os_bus_info_t *out)
{ return index == 0 && solar_os_bus_get(p, out); }
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t p, solar_os_bus_info_t *out)
{ return by_name(name) == (unsigned)p && solar_os_bus_get(p, out); }
esp_err_t solar_os_bus_acquire(const char *name, solar_os_bus_protocol_t p, const char *owner)
{ assert(by_name(name) == (unsigned)p && strstr(owner, "/native")); strcpy(owners[p], owner); ++acquired; return ESP_OK; }
esp_err_t solar_os_bus_release(const char *name, solar_os_bus_protocol_t p, const char *owner)
{ assert(by_name(name) == (unsigned)p && !strcmp(owner, owners[p])); if (fail_release) return ESP_ERR_TIMEOUT; ++released; return ESP_OK; }
esp_err_t solar_os_bus_i2c_set_speed(const char *name, uint32_t speed) { assert(by_name(name) == 0 && speed == 400000); return ESP_OK; }
esp_err_t solar_os_bus_i2c_probe(const char *name, uint8_t address) { assert(by_name(name) == 0 && address == 127); ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_i2c_receive(const char *name, uint8_t address, uint8_t *data, size_t size)
{ assert(by_name(name) == 0 && address == 127 && size == 3); memcpy(data, "rx!", 3); ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_i2c_transmit(const char *name, uint8_t address, const uint8_t *data, size_t size)
{ assert(by_name(name) == 0 && address == 127 && size == 3 && !memcmp(data, "tx!", 3)); ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_i2c_transmit_receive(const char *name, uint8_t address, const uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size)
{ assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_BUSY); solar_os_bus_i2c_transmit(name, address, tx, tx_size); return solar_os_bus_i2c_receive(name, address, rx, rx_size); }
esp_err_t solar_os_bus_i2c_read_reg(const char *name, uint8_t address, uint8_t reg, uint8_t *data, size_t size)
{ assert(reg == 255); return solar_os_bus_i2c_receive(name, address, data, size); }
esp_err_t solar_os_bus_i2c_write_reg(const char *name, uint8_t address, uint8_t reg, const uint8_t *data, size_t size)
{ assert(reg == 255); return solar_os_bus_i2c_transmit(name, address, data, size); }
esp_err_t solar_os_bus_uart_read(const char *name, uint8_t *data, size_t size, uint32_t timeout, size_t *out)
{ assert(by_name(name) == 2 && size >= 3 && timeout == 42); memcpy(data, "rx!", 3); *out = 3; ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_uart_write(const char *name, const uint8_t *data, size_t size, size_t *out)
{ assert(by_name(name) == 2 && size == 3 && !memcmp(data, "tx!", 3)); *out = size; ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_midi_read(const char *name, uint8_t *data, size_t size, uint32_t timeout, size_t *out)
{ assert(by_name(name) == 5); return solar_os_bus_uart_read("b2", data, size, timeout, out); }
esp_err_t solar_os_bus_midi_write(const char *name, const uint8_t *data, size_t size, size_t *out)
{ assert(by_name(name) == 5); return solar_os_bus_uart_write("b2", data, size, out); }
esp_err_t solar_os_bus_uart_set_baud_rate(const char *name, uint32_t baud, const char *owner)
{ assert(by_name(name) == 2 && baud == 115200 && !strcmp(owner, owners[2])); return ESP_OK; }
esp_err_t solar_os_bus_uart_autobaud_start(const char *name, const char *owner)
{ assert(by_name(name) == 2 && !strcmp(owner, owners[2])); return ESP_OK; }
esp_err_t solar_os_bus_uart_autobaud_finish(const char *name, const char *owner, solar_os_bus_uart_autobaud_result_t *out)
{ assert(by_name(name) == 2 && !strcmp(owner, owners[2])); *out = (solar_os_bus_uart_autobaud_result_t){115200, 115199, 500}; return fail_finish ? ESP_ERR_INVALID_RESPONSE : ESP_OK; }
esp_err_t solar_os_bus_uart_autobaud_cancel(const char *name, const char *owner)
{ assert(by_name(name) == 2 && !strcmp(owner, owners[2])); if (fail_cancel) return ESP_ERR_TIMEOUT; ++cancelled; return ESP_OK; }
esp_err_t solar_os_bus_onewire_reset(const char *name, bool *present) { assert(by_name(name) == 3); *present = true; return ESP_OK; }
esp_err_t solar_os_bus_onewire_scan(const char *name, uint64_t *addresses, size_t capacity, size_t *count)
{ assert(by_name(name) == 3 && capacity >= 1); addresses[0] = UINT64_C(0xfedcba9876543210); *count = 1; return ESP_OK; }
esp_err_t solar_os_bus_onewire_transfer(const char *name, const uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size)
{ assert(by_name(name) == 3 && tx_size == 3 && rx_size == 3 && !memcmp(tx, "tx!", 3)); memcpy(rx, "rx!", 3); ++transfers; return ESP_OK; }
esp_err_t solar_os_bus_spi_transfer(const char *name, int cs, uint8_t mode, uint32_t speed, const uint8_t *tx, uint8_t *rx, size_t size)
{ assert(by_name(name) == 1 && cs == -1 && mode == 3 && speed == 2000000 && size == 3 && !memcmp(tx, "tx!", 3)); memcpy(rx, "rx!", 3); ++transfers; return ESP_OK; }
int main(void)
{
    api = solar_os_native_bus_get_service("bus", 1, sizeof(*api)); assert(api);
    assert(api->runtime_available(SOLAR_OS_NATIVE_BUS_I2C) && !api->runtime_available(SOLAR_OS_NATIVE_BUS_PS2));
    int32_t port; assert(!api->endpoint(SOLAR_OS_NATIVE_BUS_SPI, 0, &port) && port == 3);
    for (unsigned p = 0; p < 6; ++p) {
        solar_os_native_bus_definition_v1_t definition = {.protocol = p, .origin = SOLAR_OS_NATIVE_BUS_RUNTIME, .sharing = SOLAR_OS_NATIVE_BUS_SHARED,
            .config = {.port = 1, .sda_pin = 7, .scl_pin = 8, .speed_hz = 400000, .host = 3, .sclk_pin = 9, .miso_pin = -1, .mosi_pin = 10,
                .max_transfer_size = 4096, .cs_count = 1, .cs = {{"select", 11}}, .tx_pin = 12, .rx_pin = 13, .baud_rate = 115200,
                .onewire_pin = 14, .ps2_clock_pin = 15, .ps2_data_pin = 16}};
        snprintf(definition.name, sizeof(definition.name), "b%u", p); assert(!api->register_bus(&definition));
        solar_os_native_bus_info_v1_t info; assert(!api->find(definition.name, p, &info) && info.definition.protocol == p && info.id == 99);
        if (p == SOLAR_OS_NATIVE_BUS_SPI) assert(info.definition.config.miso_pin == -1 && info.definition.config.cs_count == 1 && info.definition.config.cs[0].pin == 11 && !strcmp(info.definition.config.cs[0].name, "select"));
        if (p == SOLAR_OS_NATIVE_BUS_I2C) assert(info.definition.config.sda_pin == 7 && info.definition.config.speed_hz == 400000);
        assert(!api->acquire(definition.name, p, "native", &current)); char rx[4] = {0}; size_t size;
        switch (p) {
        case SOLAR_OS_NATIVE_BUS_I2C:
            assert(api->i2c_probe(current, 128) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !transfers);
            assert(!api->i2c_exchange(current, 127, "tx!", 3, rx, 3) && !memcmp(rx, "rx!", 3));
            assert(!api->i2c_read_reg(current, 127, 255, rx, 3)); break;
        case SOLAR_OS_NATIVE_BUS_SPI:
            assert(api->spi_exchange(current, -1, 4, 2000000, "tx!", rx, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            assert(!api->spi_exchange(current, -1, 3, 2000000, "tx!", rx, 3));
            assert(api->i2c_probe(current, 127) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND); break;
        case SOLAR_OS_NATIVE_BUS_UART: {
            assert(!api->uart_baud(current, 115200)); assert(!api->autobaud_start(current));
            solar_os_native_autobaud_v1_t value; fail_finish = true; assert(api->autobaud_finish(current, &value) == SOLAR_OS_NATIVE_ERROR_FAILED);
            fail_cancel = true; assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
            fail_cancel = false; break;
        }
        case SOLAR_OS_NATIVE_BUS_ONEWIRE: {
            bool present; uint64_t addresses[1]; size_t count; assert(!api->onewire_reset(current, &present) && present);
            assert(!api->onewire_scan(current, addresses, 1, &count) && count == 1 && addresses[0] == UINT64_C(0xfedcba9876543210)); break;
        }
        case SOLAR_OS_NATIVE_BUS_MIDI: assert(!api->uart_write(current, "tx!", 3, &size) && size == 3); break;
        default: break;
        }
        fail_release = true; assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        fail_release = false; assert(!api->close(current)); assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    }
    assert(acquired == 6 && released == 6 && cancelled == 1 && !allocations && !solar_os_native_handle_count());
    puts("native bus protocol values, transfers, lease protection and autobaud cleanup passed"); return 0;
}
