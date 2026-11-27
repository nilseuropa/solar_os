#include "solar_os_native_bus.h"
#include "solar_os_native_bus_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_native_runtime.h"
#endif
#include <stdio.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_buses.h"
#if SOLAR_OS_PACKAGE_SERVICE_PS2
#include "drivers/ps2_bus.h"
#include "solar_os_ps2_keyboard.h"
#include "solar_os_ps2_mouse.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_UART
#include "solar_os_uart.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_SPI
#include "solar_os_spi.h"
#include "solar_os_resources.h"
#include <stdatomic.h>
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
#include "solar_os_onewire.h"
#endif
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
        c->speed_hz = SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ;
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
static const char *bus_label(uint32_t field, uint32_t value)
{
    switch (field) {
    case SOLAR_OS_NATIVE_BUS_LABEL_PROTOCOL: {
        solar_os_bus_protocol_t protocol;
        return protocol_in(value, &protocol) ? NULL : solar_os_bus_protocol_name(protocol);
    }
    case SOLAR_OS_NATIVE_BUS_LABEL_ORIGIN:
        if (value == SOLAR_OS_NATIVE_BUS_BOARD) return solar_os_bus_origin_name(SOLAR_OS_BUS_ORIGIN_BOARD);
        if (value == SOLAR_OS_NATIVE_BUS_RUNTIME) return solar_os_bus_origin_name(SOLAR_OS_BUS_ORIGIN_RUNTIME);
        return NULL;
    case SOLAR_OS_NATIVE_BUS_LABEL_SHARING:
        if (value == SOLAR_OS_NATIVE_BUS_SHARED) return solar_os_bus_sharing_name(SOLAR_OS_BUS_SHARED);
        if (value == SOLAR_OS_NATIVE_BUS_EXCLUSIVE) return solar_os_bus_sharing_name(SOLAR_OS_BUS_EXCLUSIVE);
        return NULL;
    default:
        return NULL;
    }
}
typedef struct {
    char name[16], owner[24];
    solar_os_bus_protocol_t protocol;
    bool acquired, autobaud;
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    atomic_bool spi_exchanging;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PS2
    solar_os_ps2_bus_t *ps2;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    void *driver_pin;
#endif
} native_bus_t;

#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
static bool driver_owns_bus(const char *owner, uint32_t protocol, const char *name)
{
    solar_os_expansion_binding_kind_t kind;
    switch (protocol) {
    case SOLAR_OS_NATIVE_BUS_I2C: kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS; break;
    case SOLAR_OS_NATIVE_BUS_SPI: kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS; break;
    case SOLAR_OS_NATIVE_BUS_UART: kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT; break;
    case SOLAR_OS_NATIVE_BUS_PS2: kind = SOLAR_OS_EXPANSION_BINDING_PS2_BUS; break;
    default: return false;
    }
    return solar_os_native_driver_has_binding(owner, kind, name);
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PS2
static int ps2_disconnect(native_bus_t *state)
{
    if (!state->ps2) return SOLAR_OS_NATIVE_OK;
    const esp_err_t error = solar_os_ps2_bus_stop_checked(state->ps2);
    if (error == ESP_OK) {
        solar_os_memory_free(state->ps2);
        state->ps2 = NULL;
    }
    return solar_os_native_result(error);
}
#endif

static int bus_destroy(void *value)
{
    native_bus_t *state = value;
#if SOLAR_OS_PACKAGE_SERVICE_PS2
    const int ps2_error = ps2_disconnect(state);
    if (ps2_error) return ps2_error;
#endif
    if (state->autobaud) { esp_err_t error = solar_os_bus_uart_autobaud_cancel(state->name, state->owner); if (error) return solar_os_native_result(error); state->autobaud = false; }
    if (state->acquired) { esp_err_t error = solar_os_bus_release(state->name, state->protocol, state->owner); if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error); }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (state->driver_pin) solar_os_native_driver_unpin(state->driver_pin);
#endif
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int bus_acquire(const char *name, uint32_t protocol, const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (!name || !*name || strlen(name) >= 16 || !owner || !*owner || strlen(owner) >= 24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_bus_protocol_t p; int result = protocol_in(protocol, &p); if (result) return result;
    native_bus_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.bus"); if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    atomic_init(&state->spi_exchanging, false);
#endif
    strcpy(state->name, name); state->protocol = p; result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_BUS, state, bus_destroy, out);
    if (result) { bus_destroy(state); return result; }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    if (driver_owns_bus(owner, protocol, name)) state->driver_pin = solar_os_native_driver_pin();
    if (state->driver_pin) strcpy(state->owner, owner);
    else
#endif
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
static int uart_set_mode(solar_os_native_handle_t id, solar_os_uart_mode_t mode)
{
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_UART, false);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const int result = state->autobaud ? SOLAR_OS_NATIVE_ERROR_BUSY :
        solar_os_native_result(solar_os_bus_uart_set_mode(state->name, mode, state->owner));
    solar_os_native_handle_release(id);
    return result;
}

static int uart_status(const char *name, solar_os_uart_status_t *status)
{
    if (!status) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *status = (solar_os_uart_status_t){0};
    if (name && !*name) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_UART
    const esp_err_t error = solar_os_uart_copy_bus_status(name ? name : SOLAR_OS_UART_PORT_NAME, status);
    if (!name && error == ESP_ERR_NOT_FOUND) {
        solar_os_uart_get_status(status);
        return SOLAR_OS_NATIVE_OK;
    }
    if (error == ESP_ERR_TIMEOUT) return SOLAR_OS_NATIVE_ERROR_BUSY;
    return solar_os_native_result(error);
#else
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
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
#if SOLAR_OS_PACKAGE_SERVICE_SPI
#define SPI_DEVICE_KIND SOLAR_OS_NATIVE_HANDLE_SPI_DEVICE

typedef struct {
    spi_device_handle_t device;
    solar_os_native_handle_t bus;
    char claim_owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    uint32_t max_bytes;
    bool half_duplex;
} native_spi_device_t;

static int spi_device_destroy(void *opaque)
{
    native_spi_device_t *state = opaque;
    if (state->device) {
        const esp_err_t err = spi_bus_remove_device(state->device);
        if (err != ESP_OK) return solar_os_native_result(err);
    }
    if (state->claim_owner[0]) (void)solar_os_resource_release_owner(state->claim_owner);
    solar_os_native_handle_release(state->bus);
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static bool owns_spi_select(const native_bus_t *bus, int32_t cs)
{
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    solar_os_resource_claim_t claim;
    return bus->driver_pin &&
        solar_os_resource_find_claim(SOLAR_OS_RESOURCE_SPI_CS, cs, -1, &claim) &&
        !strcmp(claim.owner, bus->owner) &&
        solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, cs, -1, &claim) &&
        !strcmp(claim.owner, bus->owner);
#else
    (void)bus; (void)cs;
    return false;
#endif
}

static esp_err_t spi_claim_select(const native_bus_t *bus, solar_os_native_handle_t id,
    int32_t cs, char claim_owner[SOLAR_OS_RESOURCE_OWNER_MAX])
{
    if (cs < 0 || owns_spi_select(bus, cs)) return ESP_OK;
    const solar_os_resource_request_t requests[] = {
        {.kind = SOLAR_OS_RESOURCE_SPI_CS, .primary = cs, .secondary = -1, .label = bus->name},
        {.kind = SOLAR_OS_RESOURCE_GPIO_PIN, .primary = cs, .secondary = -1, .label = "spi-cs"},
    };
    snprintf(claim_owner, SOLAR_OS_RESOURCE_OWNER_MAX, "spi:%08lx", (unsigned long)id);
    return solar_os_resource_claim_bundle(requests, sizeof(requests) / sizeof(requests[0]), claim_owner, NULL);
}

static int spi_device_open(solar_os_native_handle_t lease, const solar_os_native_spi_device_config_v1_t *config,
    solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0;
    const uint32_t supported = SOLAR_OS_NATIVE_SPI_HALF_DUPLEX | SOLAR_OS_NATIVE_SPI_SINGLE_DATA_LINE |
        SOLAR_OS_NATIVE_SPI_LSB_FIRST | SOLAR_OS_NATIVE_SPI_NO_AUTO_DUMMY;
    if (!config || config->cs_pin < -1 || config->mode > 3 || !config->speed_hz ||
        config->speed_hz > 80000000U || (config->flags & ~supported) ||
        ((config->flags & SOLAR_OS_NATIVE_SPI_SINGLE_DATA_LINE) &&
            !(config->flags & SOLAR_OS_NATIVE_SPI_HALF_DUPLEX))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_bus_t *bus = bus_get(lease, SOLAR_OS_BUS_PROTOCOL_SPI, false);
    if (!bus) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_bus_info_t info;
    if (!bus->acquired || !solar_os_bus_find(bus->name, SOLAR_OS_BUS_PROTOCOL_SPI, &info)) goto release;
    const solar_os_bus_spi_config_t *spi = &info.config.spi;
    if (config->cs_pin >= 0) {
        bool registered = false;
        for (size_t i = 0; i < spi->cs_count; ++i)
            if (spi->cs[i].pin == config->cs_pin) registered = true;
        if (!registered) {
            result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
            goto release;
        }
    }
    native_spi_device_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.bus.spi");
    if (!state) {
        result = SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
        goto release;
    }
    state->bus = lease;
    state->max_bytes = spi->max_transfer_size;
    state->half_duplex = (config->flags & SOLAR_OS_NATIVE_SPI_HALF_DUPLEX) != 0;
    solar_os_native_handle_t id;
    result = solar_os_native_handle_create(SPI_DEVICE_KIND, state, spi_device_destroy, &id);
    if (result) {
        solar_os_memory_free(state);
        goto release;
    }
    const spi_device_interface_config_t device_config = {
        .spics_io_num = config->cs_pin, .clock_speed_hz = (int)config->speed_hz, .mode = config->mode,
        .queue_size = 1,
        .flags = (state->half_duplex ? SPI_DEVICE_HALFDUPLEX : 0U) |
            (config->flags & SOLAR_OS_NATIVE_SPI_SINGLE_DATA_LINE ? SPI_DEVICE_3WIRE : 0U) |
            (config->flags & SOLAR_OS_NATIVE_SPI_LSB_FIRST ? SPI_DEVICE_BIT_LSBFIRST : 0U) |
            (config->flags & SOLAR_OS_NATIVE_SPI_NO_AUTO_DUMMY ? SPI_DEVICE_NO_DUMMY : 0U),
    };
    esp_err_t err = spi_claim_select(bus, id, config->cs_pin, state->claim_owner);
    if (err == ESP_OK) err = solar_os_bus_spi_add_device(bus->name, &device_config, &state->device);
    if (err == ESP_OK) *out = id;
    else (void)solar_os_native_handle_close_kind(id, SPI_DEVICE_KIND);
    return solar_os_native_result(err);

release:
    solar_os_native_handle_release(lease);
    return result;
}

static int spi_device_exchange(solar_os_native_handle_t id, const solar_os_native_spi_exchange_v1_t *exchange)
{
    if (!exchange || exchange->command_bits > 16U || exchange->address_bits > 64U ||
        (!!exchange->tx != !!exchange->tx_bits) || (!!exchange->rx != !!exchange->rx_bits) ||
        !(exchange->command_bits || exchange->address_bits || exchange->dummy_bits ||
          exchange->tx_bits || exchange->rx_bits)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    void *opaque;
    int result = solar_os_native_handle_acquire_exclusive(id, SPI_DEVICE_KIND, &opaque);
    if (result) return result;
    const native_spi_device_t *state = opaque;
    const size_t bits = exchange->tx_bits > exchange->rx_bits ? exchange->tx_bits : exchange->rx_bits;
    if ((bits / 8U + (bits % 8U != 0U)) > state->max_bytes) result = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    else if (!state->half_duplex && exchange->tx && exchange->rx_bits > exchange->tx_bits)
        result = SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    else {
        spi_transaction_ext_t transaction = {
            .base = {
                .flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY,
                .cmd = exchange->command, .addr = exchange->address,
                .length = state->half_duplex ? exchange->tx_bits : bits, .rxlength = exchange->rx_bits,
                .tx_buffer = exchange->tx, .rx_buffer = exchange->rx,
            },
            .command_bits = exchange->command_bits, .address_bits = exchange->address_bits,
            .dummy_bits = exchange->dummy_bits,
        };
        result = solar_os_native_result(spi_device_polling_transmit(state->device, &transaction.base));
    }
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int spi_device_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SPI_DEVICE_KIND); }
#endif

static int spi_exchange(solar_os_native_handle_t id, int32_t cs, uint32_t mode, uint32_t speed, const void *tx, void *rx, size_t size)
{
    if (mode > 3) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_bus_t *state = bus_get(id, SOLAR_OS_BUS_PROTOCOL_SPI, false); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    if (atomic_exchange(&state->spi_exchanging, true)) {
        solar_os_native_handle_release(id);
        return SOLAR_OS_NATIVE_ERROR_BUSY;
    }
    char claim_owner[SOLAR_OS_RESOURCE_OWNER_MAX] = {0};
    esp_err_t error = cs < 0 ? ESP_ERR_INVALID_ARG : spi_claim_select(state, id, cs, claim_owner);
    if (error == ESP_OK) {
        error = solar_os_bus_spi_transfer(state->name, cs, mode, speed, tx, rx, size);
        if (claim_owner[0]) (void)solar_os_resource_release_owner(claim_owner);
    }
    atomic_store(&state->spi_exchanging, false);
#else
    const esp_err_t error = solar_os_bus_spi_transfer(state->name, cs, mode, speed, tx, rx, size);
#endif
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}
static int spi_resolve_cs(const char *name, const char *chip_select, int32_t *pin)
{
    if (!pin) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    int resolved;
    int result = solar_os_native_result(solar_os_spi_resolve_bus_cs(name, chip_select, &resolved));
    if (!result) *pin = resolved;
    return result;
#else
    (void)name; (void)chip_select;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
static int spi_transfer_once(const char *name, const char *owner, int32_t cs, uint32_t mode,
                             uint32_t speed, const void *tx, void *rx, size_t size)
{
    if (mode > 3) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    return solar_os_native_result(solar_os_spi_transfer_bus(name, owner, cs, mode, speed, tx, rx, size));
#else
    (void)name; (void)owner; (void)cs; (void)speed; (void)tx; (void)rx; (void)size;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}
static int onewire_reset_gpio(int32_t pin, bool *present)
{
    if (!present) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *present = false;
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    return solar_os_native_result(solar_os_onewire_reset(pin, present));
#else
    (void)pin;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int onewire_scan_gpio(int32_t pin, uint64_t *addresses, size_t capacity, size_t *count)
{
    if (!count) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    if ((!addresses && capacity) || capacity > SOLAR_OS_NATIVE_BUS_ONEWIRE_MAX_DEVICES)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    return solar_os_native_result(solar_os_onewire_scan(pin, addresses, capacity, count));
#else
    (void)pin;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

static int onewire_exchange_gpio(int32_t pin, const void *tx, size_t tx_size, void *rx, size_t rx_size)
{
    if ((!tx && tx_size) || (!rx && rx_size) || (!tx_size && !rx_size) ||
        tx_size > SOLAR_OS_NATIVE_BUS_ONEWIRE_MAX_TRANSFER || rx_size > SOLAR_OS_NATIVE_BUS_ONEWIRE_MAX_TRANSFER)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
#if SOLAR_OS_PACKAGE_SERVICE_ONEWIRE
    return solar_os_native_result(solar_os_onewire_transfer(pin, tx, tx_size, rx, rx_size));
#else
    (void)pin;
    return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
#endif
}

#if SOLAR_OS_PACKAGE_SERVICE_PS2
static int ps2_borrow(solar_os_native_handle_t id, native_bus_t **out)
{
    void *opaque;
    const int result = solar_os_native_handle_acquire_exclusive(id, SOLAR_OS_NATIVE_HANDLE_BUS, &opaque);
    if (result) return result;
    native_bus_t *state = opaque;
    if (state->protocol != SOLAR_OS_BUS_PROTOCOL_PS2) {
        solar_os_native_handle_release_exclusive(id);
        return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    }
    *out = state;
    return SOLAR_OS_NATIVE_OK;
}

static int ps2_connect(native_bus_t *state)
{
    if (state->ps2) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(state->name, SOLAR_OS_BUS_PROTOCOL_PS2, &info)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    state->ps2 = solar_os_memory_calloc(1, sizeof(*state->ps2),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "native.bus.ps2");
    if (!state->ps2) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    const esp_err_t error = solar_os_ps2_bus_start(state->ps2, &info.config.ps2);
    if (error) (void)ps2_disconnect(state);
    return solar_os_native_result(error);
}

static int ps2_start(solar_os_native_handle_t id)
{
    native_bus_t *state;
    int result = ps2_borrow(id, &state);
    if (result) return result;
    result = ps2_connect(state);
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int ps2_read(solar_os_native_handle_t id, uint8_t *data, size_t capacity, size_t *count)
{
    if (!count || (!data && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    native_bus_t *state;
    int result = ps2_borrow(id, &state);
    if (result) return result;
    if (state->ps2 && state->ps2->running) *count = solar_os_ps2_bus_read(state->ps2, data, capacity);
    else result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int ps2_command(solar_os_native_handle_t id, uint8_t command, uint32_t timeout_ms)
{
    if (!timeout_ms) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_bus_t *state;
    int result = ps2_borrow(id, &state);
    if (result) return result;
    result = state->ps2 && state->ps2->running ?
        solar_os_native_result(solar_os_ps2_bus_command(state->ps2, command, timeout_ms)) :
        SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int ps2_stats(solar_os_native_handle_t id, solar_os_native_ps2_stats_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_native_ps2_stats_v1_t){0};
    native_bus_t *state;
    int result = ps2_borrow(id, &state);
    if (result) return result;
    if (state->ps2) solar_os_ps2_bus_get_stats(state->ps2, out);
    else result = SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    solar_os_native_handle_release_exclusive(id);
    return result;
}

static int ps2_stop(solar_os_native_handle_t id)
{
    native_bus_t *state;
    int result = ps2_borrow(id, &state);
    if (result) return result;
    result = ps2_disconnect(state);
    solar_os_native_handle_release_exclusive(id);
    return result;
}
#endif

static const solar_os_native_bus_api_v1_t bus_api = {
    .abi_version = SOLAR_OS_NATIVE_BUS_ABI, .struct_size = sizeof(bus_api), .runtime_available = runtime_available, .endpoint_count = endpoint_count, .endpoint = endpoint,
    .register_bus = register_bus, .unregister_bus = unregister_bus, .attach = attach, .detach = detach, .count = bus_count, .info = bus_info, .find = bus_find,
    .acquire = bus_acquire, .close = bus_close, .i2c_speed = i2c_speed, .i2c_probe = i2c_probe, .i2c_read = i2c_read, .i2c_write = i2c_write,
    .i2c_exchange = i2c_exchange, .i2c_read_reg = i2c_read_reg, .i2c_write_reg = i2c_write_reg, .uart_read = uart_read, .uart_write = uart_write, .uart_baud = uart_baud,
    .autobaud_start = autobaud_start, .autobaud_finish = autobaud_finish, .autobaud_cancel = autobaud_cancel,
    .onewire_reset = onewire_reset, .onewire_scan = onewire_scan, .onewire_exchange = onewire_exchange, .spi_exchange = spi_exchange,
    .uart_set_mode = uart_set_mode,
    .uart_status = uart_status,
#if SOLAR_OS_PACKAGE_SERVICE_UART
    .uart_mode_name = solar_os_uart_mode_name,
    .uart_parse_mode = solar_os_uart_parse_mode,
    .uart_baud_valid = solar_os_uart_is_valid_baud_rate,
#endif
    .label = bus_label,
    .spi_resolve_cs = spi_resolve_cs,
    .spi_transfer_once = spi_transfer_once,
#if SOLAR_OS_PACKAGE_SERVICE_SPI
    .spi_device_open = spi_device_open, .spi_device_exchange = spi_device_exchange,
    .spi_device_close = spi_device_close,
#endif
    .onewire_reset_gpio = onewire_reset_gpio,
    .onewire_scan_gpio = onewire_scan_gpio,
    .onewire_exchange_gpio = onewire_exchange_gpio,
#if SOLAR_OS_PACKAGE_SERVICE_PS2
    .ps2_start = ps2_start, .ps2_read = ps2_read, .ps2_command = ps2_command,
    .ps2_stats = ps2_stats, .ps2_stop = ps2_stop,
    .ps2_keyboard_reset = solar_os_ps2_keyboard_decoder_reset, .ps2_keyboard_decode = solar_os_ps2_keyboard_decode,
    .ps2_mouse_reset = solar_os_ps2_mouse_decoder_reset, .ps2_mouse_decode = solar_os_ps2_mouse_decode,
#endif
};
#endif
const void *solar_os_native_bus_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_bus_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
