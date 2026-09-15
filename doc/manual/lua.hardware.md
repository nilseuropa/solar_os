+++
id = "lua.hardware"
title = "Lua gpio and peripherals API"
section = "api"
summary = "GPIO and peripherals: gpio, onewire, led, adc, pwm, i2c, spi, uart, neopixel, battery, charger, sensors, GNSS, haptic, IMU, NFC"
keywords = "lua solaros api hardware gpio onewire led adc pwm i2c spi uart neopixel battery charger sensors gnss haptic imu nfc"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua gpio and peripherals API

[API overview](lua.md) · [Python gpio and peripherals](python.hardware.md)

## `solaros.battery`

- `solaros.battery`: `status` with voltage, percentage, external-power,
  `charging`, and `charging_known` fields when battery support is compiled

## `solaros.sensors`

- `list()`: return registered providers with `name`, `driver`, `temperature`,
  and `humidity` fields.
- `environment([name])`: return temperature and humidity from one named
  provider. Without a name, use a combined provider when possible, then the
  default provider for each value.
- `temperature([name])`: return degrees Celsius from the default or named
  provider, or `nil` when unavailable.
- `humidity([name])`: return relative humidity as a percentage from the default
  or named provider, or `nil` when unavailable.

```lua
for _, sensor in ipairs(solaros.sensors.list()) do
    print(sensor.name, sensor.driver)
end

print(solaros.sensors.temperature())
print(solaros.sensors.humidity())
```

## `solaros.gnss`

- `list()`: return registered receivers with `name`, `driver`, `power_control`,
  and `powered`.
- `power(enabled[, name])`: change a driver-managed receiver power rail,
  defaulting to the first receiver, and return the requested state.
- `fix([name[, timeout_ms]])`: poll a receiver, defaulting to the first one and
  a 1000 ms timeout. The result includes scaled-integer position, accuracy,
  motion, fix, satellite, and UTC fields.

```lua
local fix = solaros.gnss.fix()
if fix.valid then
    print(fix.latitude_deg_e7, fix.longitude_deg_e7)
end
```

## `solaros.haptic`

- `list()`: return registered haptic devices with `name`, `driver`, and the
  number of supported numbered `effects`.
- `play(effect[, name])`: play an effect from `1` through the device's
  reported effect count, defaulting to the first haptic device.
- `stop([name])`: stop the active effect, defaulting to the first device.

```lua
solaros.haptic.play(15)
```

## `solaros.charger`

- `list()`: return registered chargers, concrete drivers, and supported ranges.
- `status([name])`: return charger state, input flags, configured values, and
  the raw fault byte.
- `enable(enabled[, name])`: enable or disable charging.
- `set_input_limit(mA[, name])`, `set_current(mA[, name])`, and
  `set_voltage(mV[, name])`: set an exact value in the advertised range.

OTG/boost mode and battery-chemistry policy are not exposed.

```lua
local status = solaros.charger.status()
print(status.state)
```

## `solaros.nfc`

- `list()`: return registered readers with `name`, `driver`, `power_control`,
  and `powered`.
- `power(enabled[, name])`: change a driver-managed reader power rail,
  defaulting to the first reader, and return the requested state.
- `scan([name[, timeout_ms]])`: discover one collision-free NFC-A tag. The
  returned `uid` and `atqa` strings are binary-safe; `sak` is numeric and
  `technology` is `"nfca"`.

## `solaros.imu`

- `list()`: return registered motion sensors with `name`, `driver`, and boolean
  `acceleration`, `angular_velocity`, and `orientation` capabilities.
- `sample([name[, timeout_ms]])`: read one sample, defaulting to the first
  sensor and a 1000 ms timeout. The result contains `timestamp_us` and any
  available `acceleration_m_s2`, `angular_velocity_rad_s`, and `orientation`
  tables. Vectors use `x`, `y`, and `z`; orientation is a unit quaternion with
  `w`, `x`, `y`, and `z`.

```lua
local sample = solaros.imu.sample()
if sample.acceleration_m_s2 then
    print(sample.acceleration_m_s2.x,
          sample.acceleration_m_s2.y,
          sample.acceleration_m_s2.z)
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

Use `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.charger`, `solaros.sensors`, `solaros.gnss`, `solaros.haptic`, `solaros.imu`, and `solaros.nfc` for gpio and peripherals.
See `man lua` for runtime conventions and service availability.
