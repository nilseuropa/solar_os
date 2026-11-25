#include "solar_os_native_bus.h"
#include "solar_os_native_bus_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <stdio.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#define PROTOCOLS(X) X(I2C) X(SPI) X(UART) X(ONEWIRE) X(PS2) X(MIDI)
static int protocol_in(uint32_t in, solar_os_bus_protocol_t *out)
{
    switch (in) {
#define ITEM(name) case SOLAR_OS_NATIVE_BUS_##name: *out = SOLAR_OS_BUS_PROTOCOL_##name; return SOLAR_OS_NATIVE_OK;
    PROTOCOLS(ITEM)
#undef ITEM
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int protocol_out(solar_os_bus_protocol_t in, uint32_t *out)
{
    switch (in) {
#define ITEM(name) case SOLAR_OS_BUS_PROTOCOL_##name: *out = SOLAR_OS_NATIVE_BUS_##name; return SOLAR_OS_NATIVE_OK;
    PROTOCOLS(ITEM)
#undef ITEM
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
}
#undef PROTOCOLS
static bool runtime_available(uint32_t protocol)
{ solar_os_bus_protocol_t value; return !protocol_in(protocol, &value) && solar_os_bus_runtime_protocol_available(value); }
static size_t endpoint_count(uint32_t protocol)
{ solar_os_bus_protocol_t value; return protocol_in(protocol, &value) ? 0 : solar_os_bus_runtime_endpoint_count(value); }
static int endpoint(uint32_t protocol, size_t index, int32_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = -1; solar_os_bus_protocol_t value; int result = protocol_in(protocol, &value), port = -1; if (result) return result;
    if (!solar_os_bus_runtime_endpoint_get(value, index, &port)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = port; return SOLAR_OS_NATIVE_OK;
}
static int definition_in(const solar_os_native_bus_definition_v1_t *in, solar_os_bus_definition_t *out)
{
    if (!in || !memchr(in->name, 0, sizeof(in->name))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); int result = protocol_in(in->protocol, &out->protocol); if (result) return result;
    out->name = in->name;
    switch (in->origin) { case SOLAR_OS_NATIVE_BUS_BOARD: out->origin = SOLAR_OS_BUS_ORIGIN_BOARD; break;
    case SOLAR_OS_NATIVE_BUS_RUNTIME: out->origin = SOLAR_OS_BUS_ORIGIN_RUNTIME; break; default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
    switch (in->sharing) { case SOLAR_OS_NATIVE_BUS_SHARED: out->sharing = SOLAR_OS_BUS_SHARED; break;
    case SOLAR_OS_NATIVE_BUS_EXCLUSIVE: out->sharing = SOLAR_OS_BUS_EXCLUSIVE; break; default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; }
    const solar_os_native_bus_config_v1_t *c = &in->config;
    switch (out->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C: out->config.i2c = (solar_os_bus_i2c_config_t){c->port, c->sda_pin, c->scl_pin, c->speed_hz}; break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        _Static_assert(sizeof(out->config.spi.cs) / sizeof(out->config.spi.cs[0]) == sizeof(c->cs) / sizeof(c->cs[0]), "SPI CS count changed");
        if (c->cs_count > sizeof(c->cs) / sizeof(c->cs[0])) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
        out->config.spi = (solar_os_bus_spi_config_t){.host = c->host, .sclk_pin = c->sclk_pin, .miso_pin = c->miso_pin,
            .mosi_pin = c->mosi_pin, .max_transfer_size = c->max_transfer_size, .cs_count = c->cs_count};
        for (size_t i = 0; i < c->cs_count; ++i) {
            if (!memchr(c->cs[i].name, 0, sizeof(c->cs[i].name))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            _Static_assert(sizeof(out->config.spi.cs[i].name) == sizeof(c->cs[i].name), "SPI CS name capacity");
            memcpy(out->config.spi.cs[i].name, c->cs[i].name, sizeof(c->cs[i].name)); out->config.spi.cs[i].pin = c->cs[i].pin;
        }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART: case SOLAR_OS_BUS_PROTOCOL_MIDI:
        out->config.uart = (solar_os_bus_uart_config_t){c->port, c->tx_pin, c->rx_pin, c->baud_rate}; break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE: out->config.onewire.pin = c->onewire_pin; break;
    case SOLAR_OS_BUS_PROTOCOL_PS2: out->config.ps2 = (solar_os_bus_ps2_config_t){c->ps2_clock_pin, c->ps2_data_pin}; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int info_out(const solar_os_bus_info_t *in, solar_os_native_bus_info_v1_t *out)
{
    memset(out, 0, sizeof(*out)); int result = protocol_out(in->protocol, &out->definition.protocol); if (result) return result;
    _Static_assert(sizeof(in->name) == sizeof(out->definition.name), "bus name capacity"); memcpy(out->definition.name, in->name, sizeof(in->name));
    switch (in->origin) { case SOLAR_OS_BUS_ORIGIN_BOARD: out->definition.origin = SOLAR_OS_NATIVE_BUS_BOARD; break;
    case SOLAR_OS_BUS_ORIGIN_RUNTIME: out->definition.origin = SOLAR_OS_NATIVE_BUS_RUNTIME; break; default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE; }
    switch (in->sharing) { case SOLAR_OS_BUS_SHARED: out->definition.sharing = SOLAR_OS_NATIVE_BUS_SHARED; break;
    case SOLAR_OS_BUS_EXCLUSIVE: out->definition.sharing = SOLAR_OS_NATIVE_BUS_EXCLUSIVE; break; default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE; }
    out->active = in->active; out->attached = in->attached; out->detachable = in->detachable; out->ready = in->ready; out->id = in->id; out->lease_count = in->lease_count;
    solar_os_native_bus_config_v1_t *c = &out->definition.config;
    switch (in->protocol) {
    case SOLAR_OS_BUS_PROTOCOL_I2C: c->port = in->config.i2c.port; c->sda_pin = in->config.i2c.sda_pin; c->scl_pin = in->config.i2c.scl_pin; c->speed_hz = in->config.i2c.speed_hz; break;
    case SOLAR_OS_BUS_PROTOCOL_SPI:
        c->host = in->config.spi.host; c->sclk_pin = in->config.spi.sclk_pin; c->miso_pin = in->config.spi.miso_pin; c->mosi_pin = in->config.spi.mosi_pin;
        c->max_transfer_size = in->config.spi.max_transfer_size; c->cs_count = in->config.spi.cs_count;
        if (c->cs_count > 5) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
        for (size_t i = 0; i < c->cs_count; ++i) { memcpy(c->cs[i].name, in->config.spi.cs[i].name, sizeof(c->cs[i].name)); c->cs[i].pin = in->config.spi.cs[i].pin; }
        break;
    case SOLAR_OS_BUS_PROTOCOL_UART: case SOLAR_OS_BUS_PROTOCOL_MIDI:
        c->port = in->config.uart.port; c->tx_pin = in->config.uart.tx_pin; c->rx_pin = in->config.uart.rx_pin; c->baud_rate = in->config.uart.baud_rate; break;
    case SOLAR_OS_BUS_PROTOCOL_ONEWIRE: c->onewire_pin = in->config.onewire.pin; break;
    case SOLAR_OS_BUS_PROTOCOL_PS2: c->ps2_clock_pin = in->config.ps2.clock_pin; c->ps2_data_pin = in->config.ps2.data_pin; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int register_bus(const solar_os_native_bus_definition_v1_t *in)
{ solar_os_bus_definition_t out; int result = definition_in(in, &out); return result ? result : solar_os_native_result(solar_os_bus_register(&out)); }
static int unregister_bus(const char *name) { return solar_os_native_result(solar_os_bus_unregister(name)); }
static int attach(const char *name) { return solar_os_native_result(solar_os_bus_attach(name)); }
static int detach(const char *name) { return solar_os_native_result(solar_os_bus_detach(name)); }
static size_t bus_count(int32_t protocol)
{ solar_os_bus_protocol_t value; return protocol == SOLAR_OS_NATIVE_BUS_ALL ? solar_os_bus_count() : protocol_in(protocol, &value) ? 0 : solar_os_bus_count_protocol(value); }
static int bus_info(size_t index, int32_t protocol, solar_os_native_bus_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_bus_info_t info = {0}; bool found;
    if (protocol == SOLAR_OS_NATIVE_BUS_ALL) found = solar_os_bus_get(index, &info);
    else { solar_os_bus_protocol_t p; int result = protocol_in(protocol, &p); if (result) return result; found = solar_os_bus_get_protocol(p, index, &info); }
    return found ? info_out(&info, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int bus_find(const char *name, uint32_t protocol, solar_os_native_bus_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_bus_protocol_t p; int result = protocol_in(protocol, &p); if (result) return result;
    solar_os_bus_info_t info = {0}; return solar_os_bus_find(name, p, &info) ? info_out(&info, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
typedef struct { char name[16], owner[24]; solar_os_bus_protocol_t protocol; bool acquired, autobaud; } native_bus_t;
static int bus_destroy(void *value)
{
    native_bus_t *state = value;
    if (state->autobaud) { esp_err_t error = solar_os_bus_uart_autobaud_cancel(state->name, state->owner); if (error) return solar_os_native_result(error); state->autobaud = false; }
    if (state->acquired) { esp_err_t error = solar_os_bus_release(state->name, state->protocol, state->owner); if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error); }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int bus_acquire(const char *name, uint32_t protocol, const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (!name || !*name || strlen(name) >= 16 || !owner || !*owner || strlen(owner) >= 24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_bus_protocol_t p; int result = protocol_in(protocol, &p); if (result) return result;
    native_bus_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.bus"); if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(state->name, name); state->protocol = p; result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_BUS, state, bus_destroy, out);
    if (result) { bus_destroy(state); return result; }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)*out, owner);
    result = solar_os_native_result(solar_os_bus_acquire(name, p, state->owner)); if (!result) state->acquired = true;
    else if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_BUS)) *out = 0;
    return result;
}
static int bus_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_BUS); }
static native_bus_t *bus_get(solar_os_native_handle_t id, solar_os_bus_protocol_t protocol, bool midi)
{
    native_bus_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_BUS); if (!state) return NULL;
    if (state->protocol == protocol || (midi && state->protocol == SOLAR_OS_BUS_PROTOCOL_MIDI)) return state;
    solar_os_native_handle_release(id); return NULL;
}
#define I2C_CALL(code) do { native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_I2C, false); \
    if (!state) { return SOLAR_OS_NATIVE_ERROR_NOT_FOUND; } int result = solar_os_native_result(code); \
    solar_os_native_handle_release(id); return result; } while (0)
static int i2c_speed(solar_os_native_handle_t id, uint32_t speed) { I2C_CALL(solar_os_bus_i2c_set_speed(state->name, speed)); }
static int i2c_probe(solar_os_native_handle_t id, uint32_t address)
{ if (address > 127) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_probe(state->name, address)); }
static int i2c_read(solar_os_native_handle_t id, uint32_t address, void *data, size_t size)
{ if (address > 127 || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_receive(state->name, address, data, size)); }
static int i2c_write(solar_os_native_handle_t id, uint32_t address, const void *data, size_t size)
{ if (address > 127 || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_transmit(state->name, address, data, size)); }
static int i2c_exchange(solar_os_native_handle_t id, uint32_t address, const void *tx, size_t tx_size, void *rx, size_t rx_size)
{ if (address > 127 || (!tx && tx_size) || (!rx && rx_size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_transmit_receive(state->name, address, tx, tx_size, rx, rx_size)); }
static int i2c_read_reg(solar_os_native_handle_t id, uint32_t address, uint32_t reg, void *data, size_t size)
{ if (address > 127 || reg > 255 || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_read_reg(state->name, address, reg, data, size)); }
static int i2c_write_reg(solar_os_native_handle_t id, uint32_t address, uint32_t reg, const void *data, size_t size)
{ if (address > 127 || reg > 255 || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT; I2C_CALL(solar_os_bus_i2c_write_reg(state->name, address, reg, data, size)); }
#undef I2C_CALL
static int uart_read(solar_os_native_handle_t id, void *data, size_t size, uint32_t timeout, size_t *read_size)
{
    if (!read_size || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *read_size = 0; native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, true); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(state->protocol == SOLAR_OS_BUS_PROTOCOL_MIDI ? solar_os_bus_midi_read(state->name, data, size, timeout, read_size) :
        solar_os_bus_uart_read(state->name, data, size, timeout, read_size)); solar_os_native_handle_release(id); return result;
}
static int uart_write(solar_os_native_handle_t id, const void *data, size_t size, size_t *written)
{
    if (!written || (!data && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *written = 0; native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, true); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(state->protocol == SOLAR_OS_BUS_PROTOCOL_MIDI ? solar_os_bus_midi_write(state->name, data, size, written) :
        solar_os_bus_uart_write(state->name, data, size, written)); solar_os_native_handle_release(id); return result;
}
static int uart_baud(solar_os_native_handle_t id, uint32_t baud)
{
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_uart_set_baud_rate(state->name, baud, state->owner)); solar_os_native_handle_release(id); return result;
}
static int autobaud_start(solar_os_native_handle_t id)
{
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_uart_autobaud_start(state->name, state->owner)); if (!result) state->autobaud = true;
    solar_os_native_handle_release(id); return result;
}
static int autobaud_finish(solar_os_native_handle_t id, solar_os_native_autobaud_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    solar_os_bus_uart_autobaud_result_t value = {0}; int result = solar_os_native_result(solar_os_bus_uart_autobaud_finish(state->name, state->owner, &value));
    if (!result) { state->autobaud = false; *out = (solar_os_native_autobaud_v1_t){value.baud_rate, value.measured_baud_rate, value.edge_count}; }
    solar_os_native_handle_release(id); return result;
}
static int autobaud_cancel(solar_os_native_handle_t id)
{
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_uart_autobaud_cancel(state->name, state->owner)); if (!result) state->autobaud = false;
    solar_os_native_handle_release(id); return result;
}
static int onewire_reset(solar_os_native_handle_t id, bool *present)
{
    if (!present) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *present = false; native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_onewire_reset(state->name, present)); solar_os_native_handle_release(id); return result;
}
static int onewire_scan(solar_os_native_handle_t id, uint64_t *addresses, size_t capacity, size_t *count)
{
    if (!count || (!addresses && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0; native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_onewire_scan(state->name, addresses, capacity, count)); solar_os_native_handle_release(id); return result;
}
static int onewire_exchange(solar_os_native_handle_t id, const void *tx, size_t tx_size, void *rx, size_t rx_size)
{
    if ((!tx && tx_size) || (!rx && rx_size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_onewire_transfer(state->name, tx, tx_size, rx, rx_size)); solar_os_native_handle_release(id); return result;
}
static int spi_exchange(solar_os_native_handle_t id, int32_t cs, uint32_t mode, uint32_t speed, const void *tx, void *rx, size_t size)
{
    if (mode > 3) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_SPI, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_bus_spi_transfer(state->name, cs, mode, speed, tx, rx, size)); solar_os_native_handle_release(id); return result;
}
static const solar_os_native_bus_api_v1_t bus_api = {
    .abi_version = SOLAR_OS_NATIVE_BUS_ABI, .struct_size = sizeof(bus_api), .runtime_available = runtime_available, .endpoint_count = endpoint_count, .endpoint = endpoint,
    .register_bus = register_bus, .unregister_bus = unregister_bus, .attach = attach, .detach = detach, .count = bus_count, .info = bus_info, .find = bus_find,
    .acquire = bus_acquire, .close = bus_close, .i2c_speed = i2c_speed, .i2c_probe = i2c_probe, .i2c_read = i2c_read, .i2c_write = i2c_write,
    .i2c_exchange = i2c_exchange, .i2c_read_reg = i2c_read_reg, .i2c_write_reg = i2c_write_reg, .uart_read = uart_read, .uart_write = uart_write, .uart_baud = uart_baud,
    .autobaud_start = autobaud_start, .autobaud_finish = autobaud_finish, .autobaud_cancel = autobaud_cancel,
    .onewire_reset = onewire_reset, .onewire_scan = onewire_scan, .onewire_exchange = onewire_exchange, .spi_exchange = spi_exchange,
};
#endif
const void *solar_os_native_bus_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_bus_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
