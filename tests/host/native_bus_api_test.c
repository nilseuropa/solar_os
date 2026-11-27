#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_bus.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_runtime.h"
#include "solar_os_buses.h"
#include "solar_os_memory.h"
#include "solar_os_uart.h"
#include "solar_os_spi.h"
#include "solar_os_resources.h"
#include "solar_os_onewire.h"
#if SOLAR_OS_PACKAGE_SERVICE_PS2
#include "drivers/ps2_bus.h"
static bool fail_ps2_stop;
#endif
static const solar_os_native_bus_api_v1_t *api;
static solar_os_bus_info_t infos[6];
static char owners[6][24];
static unsigned allocations, transfers, acquired, released, cancelled;
static bool fail_release, fail_cancel, fail_finish;
static solar_os_native_handle_t current;
static bool driver_attaching;
static unsigned driver_refs;

bool solar_os_native_driver_has_binding(const char *owner, solar_os_expansion_binding_kind_t kind, const char *target)
{
    return driver_attaching && !strcmp(owner, "peripheral") &&
        ((kind == SOLAR_OS_EXPANSION_BINDING_UART_PORT && !strcmp(target, "b2")) ||
         (kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !strcmp(target, "b1")));
}

void *solar_os_native_driver_pin(void)
{
    assert(driver_attaching);
    ++driver_refs;
    return &driver_refs;
}

void solar_os_native_driver_unpin(void *pin)
{
    assert(pin == &driver_refs && driver_refs);
    --driver_refs;
}
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
esp_err_t solar_os_onewire_reset(int pin, bool *present)
{
    assert(present && !*present);
    if (pin == 7) return ESP_ERR_NOT_ALLOWED;
    assert(pin == 9);
    *present = true;
    return ESP_OK;
}
esp_err_t solar_os_onewire_scan(int pin, uint64_t *addresses, size_t capacity, size_t *count)
{
    assert(pin == 9 && addresses && capacity == 1 && count && !*count);
    addresses[0] = UINT64_C(0xfedcba9876543210);
    *count = 1;
    return ESP_OK;
}
esp_err_t solar_os_onewire_transfer(int pin, const uint8_t *tx, size_t tx_size, uint8_t *rx, size_t rx_size)
{
    assert(pin == 9 && tx_size == 3 && rx_size == 3 && !memcmp(tx, "tx!", 3));
    memcpy(rx, "rx!", 3);
    return ESP_OK;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SPI
static bool have_primary_spi;
static const char *expected_spi_bus;
static char chip_select_owner[SOLAR_OS_RESOURCE_OWNER_MAX];
static bool fail_spi_add, fail_spi_remove, spi_attached;
static solar_os_native_handle_t device_handle;

esp_err_t solar_os_resource_claim_bundle(const solar_os_resource_request_t *requests, size_t count,
    const char *owner, solar_os_resource_conflict_t *conflict)
{
    assert(!conflict && count == 2);
    assert(requests[0].kind == SOLAR_OS_RESOURCE_SPI_CS && requests[1].kind == SOLAR_OS_RESOURCE_GPIO_PIN);
    assert(requests[0].primary == 11 && requests[1].primary == 11);
    if (chip_select_owner[0]) return ESP_ERR_INVALID_STATE;
    strcpy(chip_select_owner, owner);
    return ESP_OK;
}

size_t solar_os_resource_release_owner(const char *owner)
{
    if (strcmp(owner, chip_select_owner)) return 0;
    chip_select_owner[0] = 0;
    return 2;
}

bool solar_os_resource_find_claim(solar_os_resource_kind_t kind, int primary, int secondary,
    solar_os_resource_claim_t *claim)
{
    assert((kind == SOLAR_OS_RESOURCE_SPI_CS || kind == SOLAR_OS_RESOURCE_GPIO_PIN) &&
        primary == 11 && secondary == -1);
    if (!chip_select_owner[0]) return false;
    strcpy(claim->owner, chip_select_owner);
    return true;
}

esp_err_t solar_os_bus_spi_add_device(const char *name, const spi_device_interface_config_t *config,
    spi_device_handle_t *device)
{
    assert(!strcmp(name, "b1") && chip_select_owner[0] && !spi_attached);
    assert(config->spics_io_num == 11 && config->mode == 3 && config->clock_speed_hz == 40000000);
    assert(config->queue_size == 1 && config->flags == (SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE | SPI_DEVICE_NO_DUMMY));
    if (fail_spi_add) return ESP_ERR_NOT_SUPPORTED;
    spi_attached = true;
    *device = &spi_attached;
    return ESP_OK;
}

esp_err_t spi_bus_remove_device(spi_device_handle_t device)
{
    assert(device == &spi_attached && spi_attached && chip_select_owner[0]);
    if (fail_spi_remove) return ESP_ERR_TIMEOUT;
    spi_attached = false;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *base)
{
    assert(device == &spi_attached && spi_attached && chip_select_owner[0]);
    const spi_transaction_ext_t *phase = (const spi_transaction_ext_t *)base;
    assert(base->flags == (SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY));
    assert(base->cmd == 0x5a && phase->command_bits == 8 && base->addr == 0x209 && phase->address_bits == 16);
    assert(phase->dummy_bits == 4 && !base->length && base->rxlength == 24 && !base->tx_buffer);
    assert(api->spi_device_close(device_handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    memcpy(base->rx_buffer, "rx!", 3);
    return ESP_OK;
}
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    assert(length < size);
    memcpy(dst, src, length + 1);
    return length;
}
esp_err_t solar_os_buses_init(void) { return ESP_OK; }
esp_err_t solar_os_bus_spi_transfer_once(const char *name, int cs, uint8_t mode, uint32_t speed,
                                        const uint8_t *tx, uint8_t *rx, size_t size, const char *owner)
{
    assert(!strcmp(name, expected_spi_bus) && !strcmp(owner, "test-spi"));
    assert(cs == 11 && mode == 3 && speed == SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ);
    assert(size == 3 && !memcmp(tx, "tx!", size));
    memcpy(rx, "rx!", size);
    return ESP_OK;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
static bool uart_busy;
static solar_os_uart_mode_t uart_mode;
#endif
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{
    assert(tag && (cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED ||
        (cls == SOLAR_OS_MEMORY_INTERNAL_CRITICAL && !strcmp(tag, "native.bus.ps2"))));
    void *p = calloc(n, size);
    if (p) ++allocations;
    return p;
}
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
{
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    if (p == SOLAR_OS_BUS_PROTOCOL_SPI && !strcmp(name, "spi0")) {
        if (!have_primary_spi || !solar_os_bus_get(p, out)) return false;
        strcpy(out->name, "spi0");
        return true;
    }
#endif
    return name && strlen(name) == 2 && name[0] == 'b' && name[1] == '0' + (int)p && solar_os_bus_get(p, out);
}
esp_err_t solar_os_bus_acquire(const char *name, solar_os_bus_protocol_t p, const char *owner)
{
    assert(by_name(name) == (unsigned)p);
    if (driver_attaching) assert(!strcmp(owner, "peripheral") && driver_refs == 1);
    else assert(strstr(owner, "/native"));
    strcpy(owners[p], owner);
    ++acquired;
    return ESP_OK;
}
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
esp_err_t solar_os_bus_uart_set_mode(const char *name, solar_os_uart_mode_t mode, const char *owner)
{
    assert(by_name(name) == 2 && !strcmp(owner, owners[2]));
    if (mode != SOLAR_OS_UART_MODE_RAW && mode != SOLAR_OS_UART_MODE_LINE) return ESP_ERR_INVALID_ARG;
#if SOLAR_OS_PACKAGE_SERVICE_UART
    uart_mode = mode;
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
#if SOLAR_OS_PACKAGE_SERVICE_UART
esp_err_t solar_os_uart_copy_bus_status(const char *name, solar_os_uart_status_t *status)
{
    if (strcmp(name, "b2")) return ESP_ERR_NOT_FOUND;
    if (uart_busy) return ESP_ERR_TIMEOUT;
    *status = (solar_os_uart_status_t){.name = "b2", .attached = true, .initialized = true,
        .port_num = 1, .rx_buffered_valid = true, .rx_buffered = 128, .mode = uart_mode};
    return ESP_OK;
}
void solar_os_uart_get_status(solar_os_uart_status_t *status)
{
    *status = (solar_os_uart_status_t){.name = "uart0", .port_num = -1, .baud_rate = 115200};
}
const char *solar_os_uart_mode_name(solar_os_uart_mode_t mode)
{ return mode == SOLAR_OS_UART_MODE_LINE ? "line" : "raw"; }
bool solar_os_uart_parse_mode(const char *name, solar_os_uart_mode_t *mode)
{ if (strcmp(name, "line")) return false; *mode = SOLAR_OS_UART_MODE_LINE; return true; }
bool solar_os_uart_is_valid_baud_rate(uint32_t baud)
{ return baud == 115200; }
#endif
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
{
    assert(by_name(name) == 1 && cs == 11 && mode == 3 && speed == 2000000 && size == 3 && !memcmp(tx, "tx!", 3));
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    assert(chip_select_owner[0] && !spi_attached);
    assert(api->spi_exchange(current, cs, mode, speed, tx, rx, size) == SOLAR_OS_NATIVE_ERROR_BUSY);
#endif
    memcpy(rx, "rx!", 3);
    ++transfers;
    return ESP_OK;
}
#if SOLAR_OS_PACKAGE_SERVICE_PS2
esp_err_t solar_os_ps2_bus_start(solar_os_ps2_bus_t *bus, const solar_os_bus_ps2_config_t *config)
{
    assert(config->clock_pin == 15 && config->data_pin == 16 && !bus->running);
    bus->running = true;
    return ESP_OK;
}
esp_err_t solar_os_ps2_bus_stop_checked(solar_os_ps2_bus_t *bus)
{
    bus->running = false;
    return fail_ps2_stop ? ESP_ERR_TIMEOUT : ESP_OK;
}
size_t solar_os_ps2_bus_read(solar_os_ps2_bus_t *bus, uint8_t *data, size_t capacity)
{
    assert(bus->running && capacity > 0);
    data[0] = 0x1c;
    bus->stats.bytes++;
    return 1;
}
esp_err_t solar_os_ps2_bus_command(solar_os_ps2_bus_t *bus, uint8_t command, uint32_t timeout)
{
    assert(bus->running && command == 0xf4 && timeout == 100);
    uint8_t key;
    size_t count = 100;
    assert(api->ps2_read(current, &key, 1, &count) == SOLAR_OS_NATIVE_ERROR_BUSY && !count);
    assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_BUSY);
    bus->stats.commands++;
    return ESP_OK;
}
void solar_os_ps2_bus_get_stats(solar_os_ps2_bus_t *bus, solar_os_ps2_bus_stats_t *stats)
{ *stats = bus->stats; }
#endif

int main(void)
{
    api = solar_os_native_bus_get_service("bus", 1, sizeof(*api)); assert(api);
    assert(!strcmp(api->label(SOLAR_OS_NATIVE_BUS_LABEL_PROTOCOL, SOLAR_OS_NATIVE_BUS_I2C), "i2c"));
    assert(!strcmp(api->label(SOLAR_OS_NATIVE_BUS_LABEL_PROTOCOL, SOLAR_OS_NATIVE_BUS_MIDI), "midi"));
    assert(!strcmp(api->label(SOLAR_OS_NATIVE_BUS_LABEL_ORIGIN, SOLAR_OS_NATIVE_BUS_RUNTIME), "runtime"));
    assert(!strcmp(api->label(SOLAR_OS_NATIVE_BUS_LABEL_SHARING, SOLAR_OS_NATIVE_BUS_EXCLUSIVE), "exclusive"));
    assert(!api->label(SOLAR_OS_NATIVE_BUS_LABEL_PROTOCOL, UINT32_MAX));
    assert(!api->label(SOLAR_OS_NATIVE_BUS_LABEL_SHARING, UINT32_MAX));
    assert(api->runtime_available(SOLAR_OS_NATIVE_BUS_I2C) && !api->runtime_available(SOLAR_OS_NATIVE_BUS_PS2));
    int32_t port; assert(!api->endpoint(SOLAR_OS_NATIVE_BUS_SPI, 0, &port) && port == 3);
    for (unsigned p = 0; p < 6; ++p) {
        solar_os_native_bus_definition_v1_t definition = {.protocol = p, .origin = SOLAR_OS_NATIVE_BUS_RUNTIME, .sharing = SOLAR_OS_NATIVE_BUS_SHARED,
            .config = {.port = 1, .sda_pin = 7, .scl_pin = 8, .speed_hz = 400000, .host = 3, .sclk_pin = 9, .miso_pin = -1, .mosi_pin = 10,
                .max_transfer_size = 4096, .cs_count = 1, .cs = {{"select", 11}}, .tx_pin = 12, .rx_pin = 13, .baud_rate = 115200,
                .onewire_pin = 14, .ps2_clock_pin = 15, .ps2_data_pin = 16}};
        snprintf(definition.name, sizeof(definition.name), "b%u", p); assert(!api->register_bus(&definition));
        solar_os_native_bus_info_v1_t info; assert(!api->find(definition.name, p, &info) && info.definition.protocol == p && info.id == 99);
        if (p == SOLAR_OS_NATIVE_BUS_SPI) {
            assert(info.definition.config.miso_pin == -1 && info.definition.config.cs_count == 1 && info.definition.config.cs[0].pin == 11 && !strcmp(info.definition.config.cs[0].name, "select"));
            assert(info.definition.config.speed_hz == SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ);
        }
        if (p == SOLAR_OS_NATIVE_BUS_I2C) assert(info.definition.config.sda_pin == 7 && info.definition.config.speed_hz == 400000);
        assert(!api->acquire(definition.name, p, "native", &current)); char rx[4] = {0}; size_t size;
        switch (p) {
        case SOLAR_OS_NATIVE_BUS_I2C:
            assert(api->i2c_probe(current, 128) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !transfers);
            assert(!api->i2c_exchange(current, 127, "tx!", 3, rx, 3) && !memcmp(rx, "rx!", 3));
            assert(!api->i2c_read_reg(current, 127, 255, rx, 3)); break;
        case SOLAR_OS_NATIVE_BUS_SPI: {
            assert(api->spi_exchange(current, -1, 4, 2000000, "tx!", rx, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            assert(!api->spi_exchange(current, 11, 3, 2000000, "tx!", rx, 3));
            assert(api->i2c_probe(current, 127) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
            int32_t pin = -1;
            assert(api->spi_resolve_cs(NULL, "select", NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            assert(api->spi_transfer_once("b1", "test-spi", 11, 256, 0, "tx!", rx, 3) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
#if SOLAR_OS_PACKAGE_SERVICE_SPI
            assert(solar_os_native_bus_has_spi_device_open(api));
            solar_os_native_spi_device_config_v1_t device_config = {.cs_pin = 11, .mode = 3,
                .speed_hz = 40000000, .flags = SOLAR_OS_NATIVE_SPI_HALF_DUPLEX |
                    SOLAR_OS_NATIVE_SPI_SINGLE_DATA_LINE | SOLAR_OS_NATIVE_SPI_NO_AUTO_DUMMY};
            device_config.cs_pin = 12;
            assert(api->spi_device_open(current, &device_config, &device_handle) ==
                SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !device_handle && !chip_select_owner[0]);
            device_config.cs_pin = 11;
            fail_spi_add = true;
            assert(api->spi_device_open(current, &device_config, &device_handle) ==
                SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !device_handle && !chip_select_owner[0]);
            fail_spi_add = false;
            assert(!api->spi_device_open(current, &device_config, &device_handle));
            assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_BUSY);
            assert(api->spi_exchange(current, 11, 3, 2000000, "tx!", rx, 3) ==
                SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
            solar_os_native_spi_exchange_v1_t exchange = {.command = 0x5a, .command_bits = 8,
                .address = 0x209, .address_bits = 16, .dummy_bits = 4, .rx = rx, .rx_bits = 24};
            exchange.rx_bits = SIZE_MAX;
            assert(api->spi_device_exchange(device_handle, &exchange) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
            exchange.rx_bits = 24;
            assert(!api->spi_device_exchange(device_handle, &exchange) && !memcmp(rx, "rx!", 3));
            fail_spi_remove = true;
            assert(api->spi_device_close(device_handle) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
            assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_BUSY && chip_select_owner[0]);
            fail_spi_remove = false;
            assert(!api->spi_device_close(device_handle) && !chip_select_owner[0]);
            assert(api->spi_device_exchange(device_handle, &exchange) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
            assert(api->spi_resolve_cs("wrong", "select", &pin) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && pin == -1);
            assert(api->spi_resolve_cs("b1", "select/", &pin) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && pin == -1);
            assert(api->spi_resolve_cs("", "select", &pin) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && pin == -1);
            assert(api->spi_resolve_cs(NULL, "12", &pin) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && pin == -1);
            assert(!api->spi_resolve_cs("b1", "select", &pin) && pin == 11);
            assert(!api->spi_resolve_cs(NULL, "0xb", &pin) && pin == 11);
            expected_spi_bus = "b1";
            assert(!api->spi_transfer_once(NULL, "test-spi", 11, 3, 0, "tx!", rx, 3));
            have_primary_spi = true;
            expected_spi_bus = SOLAR_OS_NATIVE_BUS_DEFAULT_SPI;
            assert(!api->spi_transfer_once(NULL, "test-spi", 11, 3, 0, "tx!", rx, 3) && !memcmp(rx, "rx!", 3));
            expected_spi_bus = "b1";
            assert(!api->spi_transfer_once("b1", "test-spi", 11, 3, 0, "tx!", rx, 3));
            solar_os_spi_status_t status;
            assert(!solar_os_spi_get_status(&status) && status.available && !strcmp(status.name, "spi0"));
            assert(status.default_speed_hz == info.definition.config.speed_hz);
#else
            assert(!solar_os_native_bus_has_spi_device_open(api) && !api->spi_device_exchange && !api->spi_device_close);
            assert(api->spi_resolve_cs("b1", "select", &pin) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && pin == -1);
            assert(api->spi_transfer_once(NULL, "test-spi", 11, 3, 0, "tx!", rx, 3) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
#endif
            break;
        }
        case SOLAR_OS_NATIVE_BUS_UART: {
            solar_os_uart_status_t status;
            assert(api->uart_status("", &status) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            assert(api->uart_set_mode(current, 100) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
#if SOLAR_OS_PACKAGE_SERVICE_UART
            solar_os_uart_mode_t mode = SOLAR_OS_UART_MODE_RAW;
            assert(api->uart_parse_mode("line", &mode) && mode == SOLAR_OS_UART_MODE_LINE);
            assert(!strcmp(api->uart_mode_name(mode), "line") && api->uart_baud_valid(115200));
            assert(!api->uart_set_mode(current, mode));
            assert(!api->uart_status("b2", &status) && status.mode == mode && status.rx_buffered == 128);
            uart_busy = true;
            assert(api->uart_status("b2", &status) == SOLAR_OS_NATIVE_ERROR_BUSY && !status.name[0]);
            assert(!api->uart_status(NULL, &status) && !strcmp(status.name, "uart0") && !status.initialized);
            uart_busy = false;
#else
            assert(api->uart_set_mode(current, SOLAR_OS_UART_MODE_LINE) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
            assert(api->uart_status("b2", &status) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
            assert(!api->uart_parse_mode && !api->uart_mode_name && !api->uart_baud_valid);
#endif
            assert(!api->uart_baud(current, 115200)); assert(!api->autobaud_start(current));
            assert(api->uart_set_mode(current, SOLAR_OS_UART_MODE_RAW) == SOLAR_OS_NATIVE_ERROR_BUSY);
            solar_os_native_autobaud_v1_t value; fail_finish = true; assert(api->autobaud_finish(current, &value) == SOLAR_OS_NATIVE_ERROR_FAILED);
            fail_cancel = true; assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
            fail_cancel = false; break;
        }
        case SOLAR_OS_NATIVE_BUS_ONEWIRE: {
            bool present; uint64_t addresses[1]; size_t count; assert(!api->onewire_reset(current, &present) && present);
            assert(!api->onewire_scan(current, addresses, 1, &count) && count == 1 && addresses[0] == UINT64_C(0xfedcba9876543210));
            assert(api->onewire_reset_gpio(9, NULL) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
            assert(api->onewire_scan_gpio(9, addresses, SOLAR_OS_NATIVE_BUS_ONEWIRE_MAX_DEVICES + 1, &count) ==
                   SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !count);
            assert(api->onewire_exchange_gpio(9, NULL, 0, NULL, 0) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
            assert(api->onewire_reset_gpio(7, &present) == SOLAR_OS_NATIVE_ERROR_PERMISSION && !present);
            assert(!api->onewire_reset_gpio(9, &present) && present);
            assert(!api->onewire_scan_gpio(9, addresses, 1, &count) && count == 1 && addresses[0] == UINT64_C(0xfedcba9876543210));
            assert(!api->onewire_exchange_gpio(9, "tx!", 3, rx, 3) && !memcmp(rx, "rx!", 3));
#else
            assert(api->onewire_reset_gpio(9, &present) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !present);
            assert(api->onewire_scan_gpio(9, addresses, 1, &count) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED && !count);
            assert(api->onewire_exchange_gpio(9, "tx!", 3, rx, 3) == SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
#endif
            break;
        }
        case SOLAR_OS_NATIVE_BUS_PS2: {
#if SOLAR_OS_PACKAGE_SERVICE_PS2
            uint8_t byte;
            assert(api->ps2_read(current, &byte, 1, &size) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !size);
            assert(!api->ps2_start(current));
            assert(api->ps2_start(current) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
            assert(!api->ps2_read(current, &byte, 1, &size) && size == 1);
            assert(!api->ps2_command(current, 0xf4, 100));
            solar_os_native_ps2_stats_v1_t stats;
            assert(!api->ps2_stats(current, &stats) && stats.commands == 1 && stats.bytes == 1);
            solar_os_ps2_keyboard_decoder_t decoder;
            solar_os_ps2_key_transition_t key;
            api->ps2_keyboard_reset(&decoder);
            assert(api->ps2_keyboard_decode(&decoder, byte, &key) == SOLAR_OS_PS2_DECODE_KEY);
            assert(key.usage == 0x04 && key.pressed);
            solar_os_ps2_mouse_decoder_t mouse = {.count = 4};
            solar_os_ps2_mouse_packet_t packet;
            assert(api->ps2_mouse_decode(&mouse, 0x08, &packet) == SOLAR_OS_PS2_MOUSE_DECODE_UNSUPPORTED);
            assert(mouse.count == 0);
            const unsigned before = released;
            fail_ps2_stop = true;
            assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && released == before);
            fail_ps2_stop = false;
            assert(!api->ps2_stop(current));
            assert(!api->ps2_start(current));
#else
            assert(!api->ps2_start && !api->ps2_read && !api->ps2_keyboard_decode);
#endif
            break;
        }
        case SOLAR_OS_NATIVE_BUS_MIDI:
            assert(api->uart_set_mode(current, SOLAR_OS_UART_MODE_LINE) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
            assert(!api->uart_write(current, "tx!", 3, &size) && size == 3); break;
        default: break;
        }
        fail_release = true; assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        fail_release = false; assert(!api->close(current)); assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    }
    assert(acquired == 6 && released == 6 && cancelled == 1 && !allocations && !solar_os_native_handle_count());
    driver_attaching = true;
    assert(!api->acquire("b2", SOLAR_OS_NATIVE_BUS_UART, "peripheral", &current));
    assert(driver_refs == 1 && !strcmp(owners[SOLAR_OS_BUS_PROTOCOL_UART], "peripheral"));
    driver_attaching = false;
    fail_release = true;
    assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && driver_refs == 1);
    fail_release = false;
    assert(!api->close(current) && !driver_refs && !allocations && !solar_os_native_handle_count());
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    strcpy(chip_select_owner, "peripheral");
    driver_attaching = true;
    assert(!api->acquire("b1", SOLAR_OS_NATIVE_BUS_SPI, "peripheral", &current) && driver_refs == 1);
    const solar_os_native_spi_device_config_v1_t config = {.cs_pin = 11, .mode = 3,
        .speed_hz = 40000000, .flags = SOLAR_OS_NATIVE_SPI_HALF_DUPLEX |
            SOLAR_OS_NATIVE_SPI_SINGLE_DATA_LINE | SOLAR_OS_NATIVE_SPI_NO_AUTO_DUMMY};
    assert(!api->spi_device_open(current, &config, &device_handle));
    driver_attaching = false;
    assert(api->close(current) == SOLAR_OS_NATIVE_ERROR_BUSY && driver_refs == 1);
    assert(!api->spi_device_close(device_handle) && !strcmp(chip_select_owner, "peripheral"));
    assert(!api->close(current) && !driver_refs && !allocations && !solar_os_native_handle_count());
#endif
    puts("native bus protocol values, transfers, lease protection and autobaud cleanup passed"); return 0;
}
