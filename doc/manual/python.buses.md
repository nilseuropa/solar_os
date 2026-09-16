+++
id = "python.buses"
title = "Python buses and expansion API"
section = "api"
summary = "Buses and expansion: buses, expansion"
keywords = "python solaros api buses buses expansion"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python buses and expansion API

[API overview](python.md) · [Lua buses and expansion](lua.buses.md)

## `solaros.buses`

The named-bus API discovers board-defined and runtime-created buses. It is
available when the resource service is compiled, independently of the legacy
single-board-bus `solaros.spi` module.

- Constants: `MODE0` through `MODE3`, `SPI2_HOST`, `SPI3_HOST`,
  `DEFAULT_SPEED`, and `MAX_SPEED`.
- `list()`: return all named bus dictionaries.
- `get(name)`: return one named bus dictionary or raise `OSError` when absent.
- `create_i2c(name, config)`: create a runtime I2C bus and return its dictionary.
- `create_onewire(name, config)`: create a runtime 1-Wire bus and return its dictionary.
- `create_ps2(name, config)`: create an exclusive PS/2 bus from `clock` and `data` pins.
- `create_spi(name, config)`: create a runtime SPI bus and return its dictionary.
- `create_uart(name, config)`: create a lazy runtime UART bus and return its dictionary.
- `create_midi(name, config)`: create an exclusive MIDI bus and automatically select its UART backend.
- `attach(name)`: attach a named detachable bus and reserve its endpoint and pins.
- `detach(name)`: detach an idle named bus without deleting its descriptor.
- `remove(name)`: remove an idle runtime bus. Board-defined or leased buses
  cannot be removed.
- `i2c_probe(bus, address)`: probe an address on a named I2C bus.
- `i2c_scan(bus)`: return detected addresses on a named I2C bus.
- `i2c_read_reg(bus, address, reg, length)`: read bytes from an 8-bit register.
- `i2c_write_reg(bus, address, reg, data)`: write bytes to an 8-bit register.
- `onewire_reset(bus)`: reset a named 1-Wire bus and return device presence.
- `onewire_scan(bus)`: return ROM-address dictionaries found on a named bus.
- `onewire_xfer(bus, read_length[, data])`: reset, write, and read a named bus.
- `uart_write(bus, data)`: write bytes through a named UART and return the number written.
- `uart_read(bus[, length[, timeout_ms]])`: read bytes from a named UART.
- `spi_xfer(bus, cs, data[, mode[, speed_hz]])`: perform a full-duplex named-bus
  transfer and return received bytes.
- `spi_read(bus, cs, length[, fill[, mode[, speed_hz]]])`: clock in bytes using
  the optional fill byte.
- `spi_write(bus, cs, data[, mode[, speed_hz]])`: write bytes and return the
  number written.

Bus dictionaries contain `id`, `name`, `protocol`, `origin`, `sharing`,
`attached`, `detachable`, `ready`, and `lease_count`, plus protocol-specific pins and configuration. SPI
buses include `host`, `sclk_pin`, `miso_pin`, `mosi_pin`,
`max_transfer_size`, and `cs` slot dictionaries. I2C buses include `port`,
`sda_pin`, `scl_pin`, and `speed_hz`. UART and MIDI buses include `port`, `tx_pin`,
`rx_pin`, and `baud_rate`.

Named I2C operations are present when both the resource and I2C services are
compiled. They take and release a shared bus lease automatically. The legacy
`solaros.i2c` module remains an `i2c0` shortcut.

Named OneWire operations are present when both the resource and OneWire
services are compiled. They take and release an exclusive bus lease
automatically. OneWire bus dictionaries include `pin`; the legacy
`solaros.onewire` module continues to accept a direct runtime-safe GPIO.

`create_i2c` requires `port`, `sda`, and `scl`; optional `speed_hz` defaults to
100000. `create_onewire` requires `pin`. Both validate the board runtime pin
policy and claim their signal pins until `remove(name)`.

`create_uart` requires `port`, `tx`, and `rx`; optional `baud_rate` defaults to
115200. Named UART reads and writes take an exclusive lease automatically.
Runtime descriptors are detachable and removable. Board descriptors whose
signal pins are marked releasable are detachable but never removable;
fixed-pin board descriptors reject detach. Attached buses own their hardware
endpoint and signal pins, while protocol hardware starts for the first lease.

`create_midi` requires `tx` and `rx`; optional `baud_rate` defaults to 31250.
SolarOS selects an unused board-approved UART controller. The returned `port`
is diagnostic backend information, not an input to the MIDI API.

`create_spi` accepts a configuration dictionary with required `host`, `sclk`,
`mosi`, and `cs` fields. `cs` is a list of one to four chip-select GPIOs.
Optional fields are `miso` (`None` for transmit-only) and
`max_transfer_size` (default 4096 bytes). The board validates the selected host
and all signal pins. Raw named-bus transfers take and release a temporary bus
lease automatically.

Example for a runtime-routed Waveshare SPI bus:

```python
import solaros

bus = solaros.buses.create_spi("spi1", {
    "host": solaros.buses.SPI3_HOST,
    "sclk": 1,
    "mosi": 2,
    "miso": 3,
    "cs": [17],
})
print(bus)

reply = solaros.buses.spi_xfer("spi1", "gpio17", b"\x9f\x00\x00\x00")
print(reply)

solaros.buses.remove("spi1")
```

Runtime I2C and 1-Wire examples:

```python
i2c1 = solaros.buses.create_i2c("i2c1", {
    "port": 1,
    "sda": 14,
    "scl": 15,
    "speed_hz": 100000,
})
print(solaros.buses.i2c_scan(i2c1["name"]))
solaros.buses.remove(i2c1["name"])

onewire0 = solaros.buses.create_onewire("onewire0", {"pin": 16})
print(solaros.buses.onewire_scan(onewire0["name"]))
solaros.buses.remove(onewire0["name"])

uart1 = solaros.buses.create_uart("uart1", {
    "port": 1,
    "tx": 14,
    "rx": 15,
    "baud_rate": 115200,
})
solaros.buses.uart_write(uart1["name"], b"AT\r\n")
print(solaros.buses.uart_read(uart1["name"], 64, 500))
solaros.buses.detach(uart1["name"])
solaros.buses.attach(uart1["name"])
solaros.buses.remove(uart1["name"])
```

Named I2C example:

```python
import solaros

print(solaros.buses.get("i2c0"))
print([hex(addr) for addr in solaros.buses.i2c_scan("i2c0")])
solaros.buses.i2c_probe("i2c0", 0x3c)
```

Named OneWire example for a board-defined bus:

```python
import solaros

print(solaros.buses.get("onewire0"))
print(solaros.buses.onewire_reset("onewire0"))
for device in solaros.buses.onewire_scan("onewire0"):
    print(device["address"], device["family"])
```

## `solaros.expansion`

The expansion API mirrors the `expansion` shell lifecycle when the expansion
service is compiled.

- `drivers()`: return compiled driver dictionaries with `name`, `summary`,
  `category`, `required_capabilities`, `probe_supported`, and `supported`.
- `devices()`: return active device dictionaries with `name`, `driver`,
  `origin` (`board` or `runtime`), `ready`, `autostart`, `detachable`, and
  `bindings`. Each normalized binding contains `kind`, `role`, `target`,
  `value`, and `aux`.
- `attach(driver, name, bindings)`: attach a driver using a binding dictionary.
- `detach(name)`: detach a device and release its resource claims and bus leases.

Binding dictionaries accept `spi`, `cs` (or `ce`), `i2c`, `addr`, `alt_addr`, `uart`,
`ps2`, `gpio`, `irq`, `reset` (or `rst`), `data`, `bck`, `din`, `rck`, `dc`,
`mclk`, `ws`, `dout`, `busy`, `adc`, `pwm`, `count`, `keys`, `x`, `y`, `min`,
`center`, `max`, and `deadzone`. `ps2` names an existing PS/2 bus; `x` and `y`
name scalar streams;
`keys` maps logical key names to GPIO numbers. `cs` requires `spi`; `addr` and
`alt_addr` require `i2c`. Unknown keys are rejected.

```python
import solaros

solaros.expansion.attach("pcd8544", "lcd0", {
    "spi": "spi0",
    "cs": 10,
    "dc": 4,
    "reset": 5,
})
print(solaros.expansion.devices())
solaros.expansion.detach("lcd0")
```

## Quick reference

Use `solaros.buses`, `solaros.expansion` for buses and expansion.
See `man python` for runtime conventions and service availability.
