+++
id = "lua.buses"
title = "Lua buses and expansion API"
section = "api"
summary = "Buses and expansion: buses, expansion"
keywords = "lua solaros api buses buses expansion"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua buses and expansion API

[API overview](lua.md) · [Python buses and expansion](python.buses.md)

## `solaros.buses`

- `solaros.buses`: constants `MODE0` through `MODE3`, `SPI2_HOST`, `SPI3_HOST`, `DEFAULT_SPEED`, `MAX_SPEED`; functions `list`, `get`, `create_spi`, `attach`, `detach`, `remove`, `spi_xfer`, `spi_read`, `spi_write` when the resource service is compiled; `create_i2c`, `i2c_probe`, `i2c_scan`, `i2c_read_reg`, and `i2c_write_reg` are additionally present when I2C support is compiled; `create_onewire`, `onewire_reset`, `onewire_scan`, and `onewire_xfer` are additionally present when OneWire support is compiled; `create_ps2` is present with PS/2 support; `create_uart`, `create_midi`, `uart_write`, and `uart_read` are additionally present when UART support is compiled

## `solaros.expansion`

- `solaros.expansion`: `drivers`, `devices`, `attach`, `detach` when the expansion service is compiled

## Named buses and expansion devices

`solaros.buses` discovers board-defined and runtime-created buses independently
of the legacy single-board-bus and direct-pin service tables.

- `list()` returns every bus table.
- `get(name)` returns one bus table.
- `create_i2c(name, config)` creates a runtime I2C bus and returns its table.
- `create_onewire(name, config)` creates a runtime 1-Wire bus and returns its table.
- `create_spi(name, config)` creates a runtime SPI bus and returns its table.
- `create_uart(name, config)` creates a lazy runtime UART bus and returns its table.
- `create_midi(name, config)` creates an exclusive MIDI bus and automatically selects its UART backend.
- `attach(name)` attaches a named detachable bus and reserves its endpoint and pins.
- `detach(name)` detaches an idle named bus without deleting its descriptor.
- `remove(name)` removes an idle runtime bus.
- `i2c_probe(bus, address)`, `i2c_scan(bus)`,
  `i2c_read_reg(bus, address, reg, length)`, and
  `i2c_write_reg(bus, address, reg, data)` operate on a selected named I2C bus
  when both the resource and I2C services are compiled.
- `onewire_reset(bus)`, `onewire_scan(bus)`, and
  `onewire_xfer(bus, read_len[, data])` operate on a selected registered
  OneWire bus when both the resource and OneWire services are compiled.
- `uart_write(bus, data)` and `uart_read(bus[, length[, timeout_ms]])` operate
  on a selected named UART when both the resource and UART services are compiled.
- `spi_xfer(bus, cs, data[, mode[, speed_hz]])`,
  `spi_read(bus, cs, length[, fill[, mode[, speed_hz]]])`, and
  `spi_write(bus, cs, data[, mode[, speed_hz]])` transfer on a selected named
  bus. Each raw transfer takes and releases a temporary lease automatically.

Bus tables contain `id`, `name`, `protocol`, `origin`, `sharing`, `attached`,
`detachable`, `ready`, and `lease_count`, plus protocol-specific pins and configuration. `create_spi`
requires `host`, `sclk`, `mosi`, and a one-to-four-element `cs` array. `miso`
and `max_transfer_size` are optional. I2C bus tables include `port`, `sda_pin`,
`scl_pin`, and `speed_hz`. Named I2C operations take and release a shared lease
automatically; the legacy `solaros.i2c` table remains an `i2c0` shortcut.
OneWire bus tables include `pin`. Named OneWire operations take and release an
exclusive lease automatically; `solaros.onewire` remains the direct-pin
compatibility API. UART bus tables include `port`, `tx_pin`, `rx_pin`, and
`baud_rate`; named UART I/O takes and releases an exclusive
lease automatically.

`create_i2c` requires `port`, `sda`, and `scl`; optional `speed_hz` defaults to
100000. `create_onewire` requires `pin`. Both claim their approved runtime pins
until `remove(name)`.

`create_uart` requires `port`, `tx`, and `rx`; optional `baud_rate` defaults to
115200. Runtime descriptors are detachable and removable. Board descriptors
whose signal pins are marked releasable are detachable but never removable;
fixed-pin board descriptors reject detach. Attached buses own their hardware
endpoint and signal pins, while protocol hardware starts on first lease.

`create_midi` requires `tx` and `rx`; optional `baud_rate` defaults to 31250.
SolarOS selects an unused board-approved UART controller. The returned `port`
is diagnostic backend information, not an input to the MIDI API.

```lua
local solaros = require("solaros")

local bus = solaros.buses.create_spi("spi1", {
    host = solaros.buses.SPI3_HOST,
    sclk = 1,
    mosi = 2,
    miso = 3,
    cs = {17},
})
print(bus.name, bus.origin)

local reply = solaros.buses.spi_xfer("spi1", "gpio17", "\x9f\x00\x00\x00")
print(#reply)
solaros.buses.remove("spi1")
```

```lua
local solaros = require("solaros")

local i2c1 = solaros.buses.create_i2c("i2c1", {
    port = 1,
    sda = 14,
    scl = 15,
    speed_hz = 100000,
})
print(#solaros.buses.i2c_scan(i2c1.name))
solaros.buses.remove(i2c1.name)

local onewire0 = solaros.buses.create_onewire("onewire0", {pin = 16})
print(#solaros.buses.onewire_scan(onewire0.name))
solaros.buses.remove(onewire0.name)

local uart1 = solaros.buses.create_uart("uart1", {
    port = 1,
    tx = 14,
    rx = 15,
    baud_rate = 115200,
})
solaros.buses.uart_write(uart1.name, "AT\r\n")
print(solaros.buses.uart_read(uart1.name, 64, 500))
solaros.buses.detach(uart1.name)
solaros.buses.attach(uart1.name)
solaros.buses.remove(uart1.name)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("i2c0")
print(bus.name, bus.speed_hz)
local addresses = solaros.buses.i2c_scan("i2c0")
solaros.buses.i2c_probe("i2c0", 0x3c)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("onewire0")
print(bus.name, bus.pin)
local devices = solaros.buses.onewire_scan("onewire0")
local reply = solaros.buses.onewire_xfer("onewire0", 9, "\xcc\x44")
```

`solaros.expansion.drivers()` lists compiled drivers with their categories.
`devices()` lists active devices with `name`, `driver`, `origin` (`board` or
`runtime`), `ready`,
`autostart`, `detachable`, and normalized `bindings`. Each binding contains
`kind`, `role`, `target`, `value`, and `aux`. `attach(driver, name, bindings)`
and `detach(name)` mirror the shell lifecycle. Binding tables accept `spi`,
`cs` (or `ce`), `i2c`, `addr`, `alt_addr`, `uart`, `ps2`, `gpio`, `irq`, `reset` (or
`rst`), `dc`, `busy`, `data`, `bck`, `din`, `rck`, `mclk`, `ws`, `dout`,
`adc`, `pwm`, `count`, `keys`, `x`, `y`, `min`, `center`, `max`, and
`deadzone`. `ps2` names an
existing PS/2 bus; `x` and `y` name scalar streams; `keys` maps logical key
names to GPIO numbers. `cs` requires `spi`; `addr` and `alt_addr` require
`i2c`; unknown fields are rejected.

```lua
solaros.expansion.attach("pcd8544", "lcd0", {
    spi = "spi0",
    cs = 10,
    dc = 4,
    reset = 5,
})
print(#solaros.expansion.devices())
solaros.expansion.detach("lcd0")
```

NeoPixel `set` and `fill` update a buffer; call `show` once after a batch of
changes. `clear` updates and transmits immediately.

```lua
solaros.expansion.attach("neopixel", "pixels0", {data = 1, count = 8})
solaros.neopixel.fill("pixels0", 0, 0, 8)
solaros.neopixel.set("pixels0", 3, 16, 0, 0)
solaros.neopixel.show("pixels0")
```

`solaros.spi` is a compatibility table that selects `spi0` when present,
otherwise the first registered named SPI bus. On a dynamic-only board its
`status().available` value remains false until a bus is created. `status()`
reports the selected bus pins, transfer limit, and configured chip-select
slots. `xfer(cs, data[, mode[, speed_hz]])` performs a full-duplex transaction.
`read(cs, length[, fill[, mode[, speed_hz]]])` and
`write(cs, data[, mode[, speed_hz]])` provide one-direction convenience forms.
The `cs` argument accepts a configured slot name or its numeric GPIO. Lua data
and return values are binary-safe strings. New code should address buses
explicitly through `solaros.buses.spi_*`.

## Quick reference

Use `solaros.buses`, `solaros.expansion` for buses and expansion.
See `man lua` for runtime conventions and service availability.
