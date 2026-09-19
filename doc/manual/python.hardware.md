+++
id = "python.hardware"
title = "Python gpio and peripherals API"
section = "api"
summary = "GPIO and peripherals: gpio, onewire, led, adc, pwm, i2c, spi, uart, neopixel, battery, charger, sensors, GNSS, haptic, IMU, NFC"
keywords = "python solaros api hardware gpio onewire led adc pwm i2c spi uart neopixel battery charger sensors gnss haptic imu nfc"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python gpio and peripherals API

[API overview](python.md) · [Lua gpio and peripherals](lua.hardware.md)

## `solaros.battery`

Available when the firmware includes the battery service.

- `status()`: return battery status with `voltage_mv`, `percent`,
  `percent_estimated`, `adc_calibrated`, `external_power`, `charging`, and
  `charging_known`. When `charging_known` is false, `charging` is only a trend
  estimate.

Example:

```python
import solaros

battery = solaros.battery.status()
print("{} mV, {}%".format(battery["voltage_mv"], battery["percent"]))
```

## `solaros.sensors`

Available when the firmware includes the environmental sensor service.

- `list()`: return registered providers with `name`, `driver`, `temperature`,
  and `humidity` fields.
- `environment([name])`: return `temperature_c` and `humidity_percent` from a
  named provider. Without a name, prefer one provider that supplies both
  values, then fall back to the default provider for each value.
- `temperature([name])`: return a temperature in degrees Celsius from the
  default or named provider, or `None` when unavailable.
- `humidity([name])`: return relative humidity as a percentage from the
  default or named provider, or `None` when unavailable.

Example:

```python
import solaros

for sensor in solaros.sensors.list():
    print(sensor["name"], sensor["driver"])

print(solaros.sensors.temperature())
print(solaros.sensors.humidity())
```

## `solaros.gnss`

Available when the firmware includes a GNSS receiver service.

- `list()`: return registered receivers with `name`, `driver`, `power_control`,
  and `powered`.
- `power(enabled[, name])`: change a driver-managed receiver power rail,
  defaulting to the first receiver, and return the requested state.
- `fix([name[, timeout_ms]])`: poll a receiver, defaulting to the first one and
  a 1000 ms timeout. The result contains fix and UTC validity, date and time,
  fix type, `satellites_valid` and satellite count, longitude and latitude in
  degrees times 10^7, MSL height and accuracy in millimeters, ground speed in
  millimeters per second, heading in degrees times 10^5, and position DOP
  times 100.

```python
import solaros

fix = solaros.gnss.fix()
if fix["valid"]:
    print(fix["latitude_deg_e7"], fix["longitude_deg_e7"])
```

## `solaros.haptic`

Available when the firmware includes the driver-agnostic haptic service.

- `list()`: return registered haptic devices with `name`, `driver`, and the
  number of supported numbered `effects`.
- `play(effect[, name])`: play an effect from `1` through the device's
  reported effect count, defaulting to the first haptic device.
- `stop([name])`: stop the active effect, defaulting to the first device.

```python
import solaros

solaros.haptic.play(15)
```

## `solaros.charger`

Available when the firmware includes the driver-agnostic charger service.

- `list()`: return registered chargers, concrete drivers, and the supported
  `minimum`, `maximum`, and `step` for each configurable value.
- `status([name])`: return charging state, input and power-good flags, enabled
  state, configured limits, and the driver's raw fault byte.
- `enable(enabled[, name])`: enable or disable charging.
- `set_input_limit(mA[, name])`, `set_current(mA[, name])`, and
  `set_voltage(mV[, name])`: set an exact value in the range reported by
  `list()`. Values are rejected instead of silently rounded.

OTG/boost mode and battery-chemistry policy are intentionally not exposed.

```python
import solaros

print(solaros.charger.status())
```

## `solaros.nfc`

Available when the firmware includes an NFC reader service.

- `list()`: return registered readers with `name`, `driver`, `power_control`,
  and `powered`.
- `power(enabled[, name])`: change a driver-managed reader power rail,
  defaulting to the first reader, and return the requested state.
- `scan([name[, timeout_ms]])`: discover one collision-free NFC-A tag,
  defaulting to the first reader and a 1000 ms timeout. The result contains
  binary `uid` and `atqa` values, numeric `sak`, and technology `"nfca"`.

```python
import binascii
import solaros

tag = solaros.nfc.scan()
print(binascii.hexlify(tag["uid"]))
```

## `solaros.imu`

Available when the firmware includes the motion-sensor service.

- `list()`: return registered motion sensors with `name`, `driver`, and boolean
  `acceleration`, `angular_velocity`, and `orientation` capabilities.
- `sample([name[, timeout_ms]])`: read one sample, defaulting to the first
  sensor and a 1000 ms timeout. The result contains `timestamp_us`,
  `acceleration_m_s2`, `angular_velocity_rad_s`, and `orientation`.
  Unavailable measurements are `None`. Vectors use `x`, `y`, and `z` fields;
  orientation is a unit quaternion with `w`, `x`, `y`, and `z` fields.

```python
import solaros

sample = solaros.imu.sample()
accel = sample["acceleration_m_s2"]
if accel is not None:
    print(accel["x"], accel["y"], accel["z"])
```

## `solaros.gpio`

GPIO functions expose only runtime-safe expansion pins. Use `solaros.gpio.pins()`
to inspect the active board. On SolarTerm (the Waveshare ESP32-S3-RLCD-4.2) this is GPIO1,
GPIO2, GPIO3, GPIO17, plus releasable GPIO43/GPIO44 while `uart0` is detached. On the ESP32-S3-DevKitC-1-N16R8 this is GPIO1,
GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO10, GPIO14, GPIO15, GPIO16, GPIO17,
GPIO18, GPIO21, GPIO39, GPIO40, GPIO41, GPIO42, and GPIO47. On ODROID-GO this
is GPIO4 and GPIO15. On the Elecrow CrowPanel ESP32-S3 4.2-inch E-paper this is
GPIO8, GPIO9, GPIO14, GPIO15, GPIO16, GPIO17, GPIO18, GPIO19, GPIO20, GPIO21,
and GPIO38.

- Constants: `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`.
- `pins()`: return board GPIO dictionaries with `pin`, `expansion`, `allowed`,
  `available`, `claimed`, `owner`, `policy`, `role`, `configured`, `mode`,
  `pull`, `level`, and `level_valid`. Pin policy is `free`, `releasable`, or
  `fixed`; releasable pins report `allowed=True` but become available only when
  their board bus is detached.
- `allowed(pin)`: return whether a pin can be controlled by runtime apps.
- `mode(pin)`: return one pin dictionary.
- `mode(pin, mode[, pull])`: configure an allowed pin. `mode` may be `INPUT`, `OUTPUT`, `"in"`, `"input"`, `"out"`, or `"output"`.
- `configure(pin, mode[, pull])`: alias for `mode(pin, mode[, pull])`.
- `read(pin)`: read an allowed pin and return `0` or `1`.
- `write(pin, value)`: set an allowed pin low or high. If needed, the pin is configured as output first.
- `release(pin)`: reset the pin and release its direct-GPIO claim.

Example:

```python
import solaros

for pin in solaros.gpio.pins():
    print(pin)

solaros.gpio.mode(17, solaros.gpio.INPUT, solaros.gpio.PULL_UP)
print("GPIO17", solaros.gpio.read(17))

solaros.gpio.write(1, 1)
```

## `solaros.onewire`

OneWire functions operate on runtime-safe expansion GPIOs when the OneWire
service is included in the active flavor. Use `solaros.buses.onewire_*` for a
registered named bus. Transfers reset the bus before writing and reading, and
are limited to 64 bytes in each direction.

- `allowed(pin)`: return whether the pin is available for OneWire operations.
- `reset(pin)`: reset the bus and return whether a presence pulse was detected.
- `scan(pin)`: return device dictionaries containing a 16-digit hexadecimal `address` and numeric `family` code.
- `xfer(pin, read_len[, data])`: reset the bus, write a bytes-like object, then read and return `read_len` bytes. Either `read_len` or `data` must be non-empty.

Example:

```python
import solaros

for device in solaros.onewire.scan(17):
    print(device["address"], device["family"])

# Skip ROM, issue a command, and read two response bytes.
response = solaros.onewire.xfer(17, 2, b"\xcc\x44")
print(response)
```

## `solaros.led`

Status LED functions control a built-in board status LED when the board has one.

- `status()`: return whether the status LED is currently on.
- `set(on)`: set the status LED and return the resulting boolean state.
- `on()`: turn the status LED on and return `True`.
- `off()`: turn the status LED off and return `False`.
- `toggle()`: toggle the status LED and return the resulting boolean state.

Example:

```python
import solaros

solaros.led.toggle()
```

## `solaros.adc`

ADC functions expose analog reads on runtime-safe expansion pins that are ADC
capable. Some runtime GPIOs are digital-only; check `adc_capable` from
`solaros.adc.pins()` before reading.

- `pins()`: return dictionaries with `pin`, `allowed`, `adc_capable`, `unit`, and `channel`.
- `read(pin)`: return `pin`, `raw`, `voltage_mv`, `unit`, `channel`, and `calibrated`.

Example:

```python
import solaros

print(solaros.adc.pins())
print(solaros.adc.read(1))
```

## `solaros.pwm`

PWM functions expose LEDC PWM output on runtime-safe expansion pins. Active PWM outputs share one LEDC timer, so changing the frequency changes the frequency for all active PWM outputs.

- Constants: `FREQ_MIN`, `FREQ_MAX`.
- `status()`: return dictionaries with `pin`, `allowed`, `active`, `channel`, `freq_hz`, and `duty_percent`.
- `set(pin, freq_hz, duty_percent)`: start or update PWM on a pin. Duty is `0..100`.
- `off(pin)`: stop PWM on a pin.

Example:

```python
import solaros

solaros.pwm.set(1, 1000, 50)
print(solaros.pwm.status())
solaros.pwm.off(1)
```

## `solaros.neopixel`

Available when the NeoPixel expansion package is compiled.

- `list()`: return attached strip dictionaries with `name`, `data_pin`, and `count`.
- `set(name, index, red, green, blue)`: update one buffered pixel.
- `fill(name, red, green, blue)`: update every buffered pixel.
- `show(name)`: transmit the buffered colors in GRB wire order.
- `clear(name)`: clear the buffer and transmit it immediately.

```python
import solaros

solaros.expansion.attach("neopixel", "pixels0", {"data": 1, "count": 8})
solaros.neopixel.fill("pixels0", 0, 0, 8)
solaros.neopixel.set("pixels0", 3, 16, 0, 0)
solaros.neopixel.show("pixels0")
```

## `solaros.i2c`

I2C functions expose `i2c0` for diagnostics and compatibility. Use
`solaros.buses.i2c_*` to select a named bus.

- `info()`: return bus speed and SDA/SCL pins.
- `probe(address)`: raise on missing device, return `None` on success.
- `scan()`: return detected addresses.
- `read_reg(address, reg, length)`: read bytes from an 8-bit register.
- `write_reg(address, reg, data)`: write bytes to an 8-bit register.

Example:

```python
import solaros

print(solaros.i2c.info())
print([hex(addr) for addr in solaros.i2c.scan()])
```

## `solaros.spi`

Available when the board and flavor include the SPI service. This compatibility
module selects `spi0` when present, otherwise the first registered named SPI
bus. On a dynamic-only board, `status()["available"]` remains `False` until a
bus is created. Chip select may be a configured CS name from `status()["cs"]`
or its configured numeric GPIO. Transfers are limited to the selected bus's
reported `max_transfer_size`; new code should address buses explicitly through
`solaros.buses.spi_*`.

- Constants: `MODE0`, `MODE1`, `MODE2`, `MODE3`, `DEFAULT_SPEED`, `MAX_SPEED`.
- `status()`: return the bus name, host, pins, speed, transfer limit, and configured CS slots.
- `xfer(cs, data[, mode[, speed_hz]])`: perform a full-duplex transfer and return the received bytes.
- `read(cs, length[, fill[, mode[, speed_hz]]])`: transmit the fill byte, default `0xff`, while reading.
- `write(cs, data[, mode[, speed_hz]])`: write bytes and return the number written.

Example:

```python
import solaros

status = solaros.spi.status()
cs = status["cs"][0]["name"]

# JEDEC ID command followed by three dummy bytes in one CS transaction.
response = solaros.spi.xfer(cs, b"\x9f\x00\x00\x00", solaros.spi.MODE0, 1_000_000)
print(response[1:])
```

## `solaros.uart`

UART functions expose the default `uart0` compatibility service. Use
`solaros.buses.uart_*` to address another named UART bus.

- `status()`: return UART name, `attached`, port, pins, baud rate, mode, `rx_buffered`, and `rx_buffered_valid`. When another owner is actively using the UART, `rx_buffered_valid` is `False` because the live RX count is not sampled.
- `baud([rate])`: get or set baud rate.
- `is_valid_baud(rate)`: return whether a baud rate is accepted.
- `mode([name])`: get or set `raw` or `line` mode.
- `write(data)`: write bytes and return bytes written.
- `read([length[, timeout_ms]])`: read bytes.

Example:

```python
import solaros

solaros.uart.baud(115200)
solaros.uart.mode("raw")
solaros.uart.write(b"AT\r\n")
print(solaros.uart.read(64, 500))
```

## Quick reference

Use `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.charger`, `solaros.sensors`, `solaros.gnss`, `solaros.haptic`, `solaros.imu`, and `solaros.nfc` for gpio and peripherals.
See `man python` for runtime conventions and service availability.
