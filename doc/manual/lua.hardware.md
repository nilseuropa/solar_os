+++
id = "lua.hardware"
title = "Lua gpio and peripherals API"
section = "api"
summary = "GPIO and peripherals: gpio, onewire, led, adc, pwm, i2c, spi, uart, neopixel, battery, sensors"
keywords = "lua solaros api hardware gpio onewire led adc pwm i2c spi uart neopixel battery sensors"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua gpio and peripherals API

[API overview](lua.md) · [Python gpio and peripherals](python.hardware.md)

## `solaros.battery`

- `solaros.battery`: `status` with voltage, percentage, external-power,
  `charging`, and `charging_known` fields when battery support is compiled

## `solaros.sensors`

- `solaros.sensors`: `environment` when environmental sensor support is compiled

## `solaros.gnss`

- `list()`: return registered receivers with `name` and `driver`.
- `fix([name[, timeout_ms]])`: poll a receiver, defaulting to the first one and
  a 1000 ms timeout. The result includes scaled-integer position, accuracy,
  motion, fix, satellite, and UTC fields.

```lua
local fix = solaros.gnss.fix()
if fix.valid then
    print(fix.latitude_deg_e7, fix.longitude_deg_e7)
end
```

## `solaros.gpio`

- `solaros.gpio`: constants `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`; functions `pins`, `allowed`, `mode`, `configure`, `read`, `write`, `release` when GPIO support is compiled. Pin tables include `expansion`, `allowed`, `available`, `claimed`, `owner`, and `policy` (`free`, `releasable`, or `fixed`).

## `solaros.onewire`

- `solaros.onewire`: `allowed`, `reset`, `scan`, `xfer` for the direct-pin compatibility API when OneWire support is compiled

## `solaros.led`

- `solaros.led`: `status`, `set`, `on`, `off`, `toggle` when GPIO support is compiled

## `solaros.adc`

- `solaros.adc`: `pins`, `read` when ADC support is compiled

## `solaros.pwm`

- `solaros.pwm`: constants `FREQ_MIN`, `FREQ_MAX`; functions `status`, `set`, `off` when PWM support is compiled

## `solaros.neopixel`

- `solaros.neopixel`: `list`, `set`, `fill`, `show`, `clear` when the NeoPixel expansion package is compiled

## `solaros.i2c`

- `solaros.i2c`: `info`, `probe`, `scan`, `read_reg`, `write_reg` when I2C support is compiled

## `solaros.spi`

- `solaros.spi`: constants `MODE0` through `MODE3`, `DEFAULT_SPEED`, and `MAX_SPEED`; functions `status`, `xfer`, `read`, `write` when SPI support is compiled

## `solaros.uart`

- `solaros.uart`: `status`, `baud`, `is_valid_baud`, `mode`, `write`, `read` when UART support is compiled

## Direct-pin OneWire

`solaros.onewire.scan(pin)` returns tables containing a 16-digit hexadecimal
`address` and numeric `family` code. `solaros.onewire.xfer(pin, read_len[, data])`
resets the bus, writes the binary-safe `data` string, and returns `read_len`
bytes. Reads and writes are each limited to 64 bytes.

## Default UART

`solaros.uart` is the default `uart0` compatibility table; use
`solaros.buses.uart_*` for another named UART and `solaros.buses.attach()` or
`detach()` for lifecycle control. `solaros.uart.status()`
includes the bus `name`, `attached`, `rx_buffered`, and `rx_buffered_valid`.
When another owner is
actively using the UART, `rx_buffered_valid` is `false` because the live RX
count is not sampled.

## Quick reference

Use `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.sensors`, and `solaros.gnss` for gpio and peripherals.
See `man lua` for runtime conventions and service availability.
