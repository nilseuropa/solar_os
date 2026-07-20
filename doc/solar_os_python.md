# SolarOS Python API

SolarOS embeds MicroPython as the `python` foreground application. It can run an interactive REPL or execute `.py` and `.mpy` files from storage.

```text
python
python /apps/demo.py arg1 arg2
```

Scripts receive their arguments through `sys.argv`. Script output is drawn in the SolarOS terminal. The active shell's app-exit key exits the REPL or requests `KeyboardInterrupt` while code is running.

The native module is called `solaros`:

```python
import solaros

solaros.write("SolarOS " + solaros.version() + "\n")
```

## Conventions

Most mutating functions return `None` on success and raise `OSError("ESP_ERR_...")` on service failure. Query functions return strings, integers, booleans, dictionaries, or lists.

Functions that accept file paths use SolarOS shell-style paths. `/` means the default storage mount; internally this resolves to the active storage mount point.

The Python runtime package requires PSRAM. Hardware and network helpers are
added only when the board/flavor includes their service package. For example,
an ODROID-GO full build includes Python with `solaros.spi` and
`solaros.onewire`, while omitting `solaros.adc` and `solaros.i2c` because those
service packages are not available on that board.

Optional API groups follow these package gates:

- `service.wifi`: top-level `wifi_status` and `solaros.wifi`
- `net`: `solaros.mqtt`, `solaros.net`, and `solaros.ssh_keys`
- `service.ble`: `solaros.ble`
- `service.gpio`: `solaros.gpio` and `solaros.led`
- `service.onewire`: `solaros.onewire`
- `service.adc`, `service.pwm`, `service.i2c`, `service.spi`, and
  `service.uart`: their matching submodules
- `service.audio`, `service.battery`, and `service.sensors`: their matching
  helpers and submodules

```python
print(solaros.storage.resolve("/.shell/history"))
```

Datetime dictionaries use this shape:

```python
{
    "year": 2026,
    "month": 6,
    "day": 19,
    "hour": 12,
    "minute": 30,
    "second": 0,
    "weekday": 5,
    "clock_integrity": True,
}
```

Datetime setters and converters accept either such a dict or positional values:

```python
solaros.time.set_datetime(2026, 6, 19, 12, 30, 0)
solaros.time.set_datetime({"year": 2026, "month": 6, "day": 19, "hour": 12, "minute": 30})
```

## Top-Level Helpers

- `solaros.write(text)`: write text to the SolarOS terminal.
- `solaros.version()`: return the SolarOS firmware version string.
- `solaros.should_exit()`: return `True` when the app is being asked to stop.
- `solaros.battery_status()`: shortcut for `solaros.battery.status()` when battery support is compiled.
- `solaros.wifi_status()`: compact Wi-Fi status shortcut when Wi-Fi support is compiled.
- `solaros.environment()`: shortcut for `solaros.sensors.environment()` when environmental sensor support is compiled.

## `solaros.storage`

Storage functions expose SD mount and filesystem service operations.

- `status()`: return a human-readable status string for the preferred mounted
  persistent storage (SD when mounted, otherwise internal flash).
- `is_mounted()`: return whether the default storage volume is mounted.
- `mount()`: mount the default storage volume.
- `unmount()`: unmount the default storage volume.
- `mount_point()`: return the preferred mounted persistent-storage path (SD
  when mounted, otherwise internal flash).
- `usage([path])`: return disk usage for the default volume or the volume containing `path`.
- `resolve(path)`: return the internal resolved path.
- `rescan()`: rescan SD block devices and partitions.
- `blocks()`: return a list of block device and partition dictionaries.
- `block_count()`: return the number of known blocks.
- `block(index)`: return one block dictionary.
- `usage_for_block(index)`: return usage for one mounted block.
- `mkdir(path)`: create a directory.
- `rmdir(path)`: remove an empty directory.
- `remove(path)`: remove a file.
- `rename(old_path, new_path)`: rename or move a file or directory.
- `copy(source_path, dest_path)`: copy a file.
- `mount_volume(name[, mount_point])`: mount a named block or partition.
- `unmount_volume(target)`: unmount by volume name or mount point.

Example:

```python
import solaros

if not solaros.storage.is_mounted():
    solaros.storage.mount()

print(solaros.storage.usage("/"))
for block in solaros.storage.blocks():
    print(block["name"], block["type"], block["mounted"], block["mount_point"])
```

## `solaros.time`

Time functions use the SolarOS RTC/time service.

- `uptime_ms()`: return uptime in milliseconds.
- `uptime()`: return formatted uptime text.
- `datetime()`: return local RTC datetime.
- `utc_datetime()`: return UTC datetime.
- `set_datetime(datetime)`: set local RTC datetime.
- `set_utc_datetime(datetime)`: set UTC RTC datetime.
- `utc_to_local(datetime)`: convert UTC datetime to local time.
- `local_to_utc(datetime)`: convert local datetime to UTC.
- `is_valid(datetime)`: validate a datetime.
- `timezone()`: return `{"name": ..., "posix": ...}`.
- `set_timezone(timezone)`: set timezone by SolarOS-supported name or POSIX TZ string.
- `ntp_sync([server[, timeout_ms]])`: sync RTC from NTP and return `{"utc": ..., "local": ...}`.

Example:

```python
import solaros

print("uptime", solaros.time.uptime())
print("local", solaros.time.datetime())

if solaros.wifi.status()["has_ip"]:
    print(solaros.time.ntp_sync())
```

## `solaros.battery`

Available when the firmware includes the battery service.

- `status()`: return battery status with `voltage_mv`, `percent`, `percent_estimated`, `adc_calibrated`, and `external_power`.

Example:

```python
import solaros

battery = solaros.battery.status()
print("{} mV, {}%".format(battery["voltage_mv"], battery["percent"]))
```

## `solaros.sensors`

Available when the firmware includes the environmental sensor service.

- `environment()`: return `temperature_c` and `humidity_percent`.

Example:

```python
import solaros

env = solaros.sensors.environment()
print("{:.1f} C {:.1f}%".format(env["temperature_c"], env["humidity_percent"]))
```

## `solaros.wifi`

Wi-Fi functions expose station, SoftAP, scan, and NAT controls.

- `status()`: return detailed Wi-Fi status.
- `status_text()`: return the same compact status text used by the shell.
- `start()`: start Wi-Fi and reconnect saved station config if present.
- `stop()`: stop Wi-Fi.
- `connect(ssid[, password])`: connect to a station network and save it.
- `connect_saved()`: connect using remembered station profiles.
- `disconnect()`: disconnect station mode.
- `forget()`: remove the active or preferred station profile.
- `forget_ssid(ssid)`: remove one remembered station profile.
- `forget_all()`: remove all remembered station profiles.
- `known()`: return remembered station profiles as dictionaries with `ssid` and `preferred`.
- `scan()`: return visible APs as dictionaries with `ssid`, `auth`, `rssi`, `channel`, and `hidden`.
- `ap_start([ssid[, password[, auth]]])`: start SoftAP, reusing saved AP config when no arguments are supplied.
- `ap_stop()`: stop SoftAP.
- `nat(enabled)`: persistently enable or disable APSTA NAT.

Example:

```python
import solaros

solaros.wifi.start()
print(solaros.wifi.status())

for ap in solaros.wifi.scan():
    print(ap["rssi"], ap["auth"], ap["ssid"])
```

## `solaros.mqtt`

MQTT functions expose the shared SolarOS MQTT service. Broker URL and credentials are stored in NVS, so they work without an SD card.

- `status()`: return MQTT status, saved broker URL, client ID, auth flags, counters, queued message count, and last error.
- `connect([url[, username[, password]]])`: connect to `mqtt://...` or `mqtts://...`; supplied values are saved in NVS. With no arguments, reconnect using saved settings.
- `disconnect()`: stop the MQTT client.
- `publish(topic, payload[, qos[, retain]])`: publish bytes or text and return the message ID.
- `subscribe(topic[, qos])`: subscribe and return the message ID.
- `read([timeout_ms])`: return the next queued message dictionary, or `None` on timeout. Message payloads are returned as bytes.

Example:

```python
import solaros

solaros.mqtt.connect("mqtts://broker.example.com:8883", "user", "secret")
solaros.mqtt.publish("solaros/status", b"online", 0, False)
solaros.mqtt.subscribe("solaros/inbox/#")

while not solaros.should_exit():
    msg = solaros.mqtt.read(1000)
    if msg:
        print(msg["topic"], msg["payload"])
```

## `solaros.gpio`

GPIO functions expose only runtime-safe expansion pins. Use `solaros.gpio.pins()`
to inspect the active board. On the Waveshare ESP32-S3-RLCD-4.2 this is GPIO1,
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
- `create_spi(name, config)`: create a runtime SPI bus and return its dictionary.
- `create_uart(name, config)`: create a lazy runtime UART bus and return its dictionary.
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
`sda_pin`, `scl_pin`, and `speed_hz`. UART buses include `port`, `tx_pin`,
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
  `required_capabilities`, `probe_supported`, and `supported`.
- `devices()`: return active device dictionaries and their normalized binding
  lists.
- `attach(driver, name, bindings)`: attach a driver using a binding dictionary.
- `detach(name)`: detach a device and release its resource claims and bus leases.

Binding dictionaries accept `spi`, `cs` (or `ce`), `i2c`, `addr`, `uart`,
`gpio`, `irq`, `reset` (or `rst`), `dc`, `busy`, `adc`, and `pwm`. `cs`
requires `spi`, and `addr` requires `i2c`. Unknown keys are rejected.

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

## `solaros.audio`

Available when the firmware includes the audio service.

Audio functions expose the microphone, speaker, and WAV service.

- `status()`: return codec/sample/pin status.
- `deinit()`: turn audio hardware off.
- `off()`: alias for `deinit()`.
- `set_volume(volume)`: set speaker volume.
- `set_mic_gain(gain_db)`: set microphone gain.
- `tone(frequency_hz, duration_ms[, volume])`: play a tone.
- `level(duration_ms)`: measure input level and return samples, peak, and average percent.
- `loopback(duration_ms[, volume])`: run microphone-to-speaker loopback.
- `wav_info(path)`: inspect a WAV file.
- `record_wav(path, duration_ms)`: record a native WAV file.
- `play_wav(path[, volume])`: play a native WAV file.

Example:

```python
import solaros

print(solaros.audio.status())
solaros.audio.tone(880, 200, 40)
print(solaros.audio.level(500))
```

## `solaros.ble`

BLE functions expose keyboard pairing and layout controls.

- `status()`: return human-readable BLE keyboard status.
- `connected()`: return whether a keyboard is connected.
- `pair()`: start keyboard pairing.
- `forget()`: remove remembered keyboard pairing.
- `layout([name])`: get or set keyboard layout, currently `us` or `de`.
- `read([max_bytes])`: read pending decoded keyboard bytes.

Example:

```python
import solaros

print(solaros.ble.status())
print("layout", solaros.ble.layout())
```

## `solaros.clipboard`

The clipboard is PSRAM-backed and shared with SolarOS apps that use the clipboard service.

- `set(data)`: set clipboard bytes.
- `get()`: return clipboard bytes.
- `size()`: return clipboard size in bytes.
- `clear()`: clear the clipboard.

Example:

```python
import solaros

solaros.clipboard.set(b"hello from python")
print(solaros.clipboard.get())
```

## `solaros.identity`

Identity functions read the SolarOS user and hostname service.

- `user()`: return the configured username.
- `hostname()`: return the configured hostname.
- `format()`: return `user@hostname`.

Example:

```python
import solaros

print(solaros.identity.format())
```

## `solaros.net`

- `ping(host[, count[, timeout_ms[, interval_ms[, data_size]]]])`: ping a host and return transmit/receive statistics.

Example:

```python
import solaros

print(solaros.net.ping("example-host", 4))
```

## `solaros.ssh_keys`

SSH key functions manage the default SolarOS SSH key pair.

- `default_paths()`: return private and public key paths.
- `default_exists()`: return whether both default key files exist.
- `status()`: return key existence, sizes, and paths.
- `generate([bits[, overwrite]])`: generate RSA keys.
- `remove()`: remove the default key pair.

Example:

```python
import solaros

if not solaros.ssh_keys.default_exists():
    solaros.ssh_keys.generate()

print(solaros.ssh_keys.status())
```

## `solaros.jobs`

Job functions control SolarOS background jobs.

- `list()`: return all jobs.
- `count()`: return number of jobs.
- `status(name)`: return one job status.
- `start(name[, args])`: start a job; `args` is a list or tuple of strings.
- `stop(name)`: stop a job.

Example:

```python
import solaros

solaros.jobs.start("ntp-sync", ["60", "pool.ntp.org"])
print(solaros.jobs.status("ntp-sync"))
solaros.jobs.stop("ntp-sync")
```

## `solaros.sessions`

Session functions create and close foreground shell/app sessions.

- `create_shell(port[, term[, cols, rows]])`: create a port shell session and return its numeric session id.
- `create_shell(port, term="auto", cols=80, rows=24)`: keyword form for the same call.
- `close(session_id)`: close a display/app session or stop a port shell session.

Manual port shell sessions created from scripts do not run `/.shell/startup`.

Example:

```python
import solaros

try:
    solaros.jobs.stop("slip")
except OSError:
    pass

sid = solaros.sessions.create_shell("uart0", term="auto")
# later:
solaros.sessions.close(sid)
solaros.jobs.start("slip", ["uart0", "115200"])
```

## `solaros.apps`

Application functions inspect the built-in foreground app registry.

- `list()`: return registered apps with `name` and `summary`.
- `find(name)`: return one app dictionary or `None`.

Example:

```python
import solaros

for app in solaros.apps.list():
    print(app["name"], "-", app["summary"])
```

## `solaros.tui`

TUI functions provide a small curses-like text UI layer over the SolarOS terminal. Drawing calls are queued onto the foreground UI side, so Python scripts do not write terminal memory directly.

Attributes:

- `NORMAL`
- `BOLD`
- `INVERSE`

Functions:

- `rows()`: return terminal rows.
- `cols()`: return terminal columns.
- `size()`: return `(rows, cols)`.
- `clear()`: clear the terminal.
- `refresh()`: force a display refresh.
- `move(row, col)`: move the terminal cursor.
- `write(text[, attr])`: write at the current cursor.
- `addstr(row, col, text[, attr])`: move and write text.
- `putch(row, col, ch[, attr])`: draw one character or codepoint.
- `hline(row, col, width[, attr])`: draw a horizontal line.
- `vline(row, col, height[, attr])`: draw a vertical line.
- `vrule(row, col, height[, width[, attr]])`: draw a continuous pixel vertical rule.
- `box(row, col, height, width[, attr])`: draw a box.
- `fill(row, col, height, width[, ch[, attr]])`: fill a rectangle.
- `getch([timeout_ms])`: return a key code or `None`.

Common key constants include `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_HOME`, `KEY_END`, `KEY_DELETE`, `KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Example:

```python
import solaros
from solaros import tui

rows, cols = tui.size()
tui.clear()
tui.box(0, 0, rows, cols)
tui.addstr(1, 2, "SolarOS TUI", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit():
    key = tui.getch(250)
    if key == tui.KEY_ESCAPE:
        break
```

## `solaros.gfx`

Graphics functions provide queued access to the SolarOS foreground graphics service. Call `begin()` before drawing and `refresh()`/`present()` to push the frame to the display. `begin(target)` claims a named display target, such as `lcd0`, until `end()` or script cleanup.

Colors:

- `WHITE`
- `LIGHT`
- `DARK`
- `BLACK`
- `GRAY_MAX`: maximum grayscale level accepted by `gray(level)`, currently `16`.

`gray(level)` returns an encoded grayscale color. Level `0` is black and `GRAY_MAX` is white. Intermediate levels are rendered with ordered spatial dithering on the reflective 1-bit framebuffer.

Fonts:

- `FONT_SMALL`
- `FONT_MONO`
- `FONT_BOLD`
- `FONT_MONO_12`, `FONT_MONO_14`, `FONT_MONO_16`, `FONT_MONO_18`, `FONT_MONO_20`
- `FONT_BOLD_12`, `FONT_BOLD_14`, `FONT_BOLD_16`, `FONT_BOLD_18`, `FONT_BOLD_20`
- `FONT_ITALIC_12`, `FONT_ITALIC_14`, `FONT_ITALIC_16`, `FONT_ITALIC_18`, `FONT_ITALIC_20`
- `FONT_BOLD_ITALIC_12`, `FONT_BOLD_ITALIC_14`, `FONT_BOLD_ITALIC_16`, `FONT_BOLD_ITALIC_18`, `FONT_BOLD_ITALIC_20`

Italic constants currently map to the closest upright face in the trimmed firmware font set.

Functions:

- `begin([target])`: enter foreground graphics mode; when `target` is provided, claim and draw to that named display target.
- `end()`: leave graphics mode and redraw the terminal.
- `width()`: return graphics width in pixels.
- `height()`: return graphics height in pixels.
- `size()`: return `(width, height)`.
- `clear([color])`: clear the graphics buffer, defaulting to `WHITE`.
- `gray(level)`: return a grayscale color value from `0` to `GRAY_MAX`.
- `color([color])`: get or set current drawing color.
- `set_color(color)`: alias for `color(color)`.
- `font([font])`: get or set current text font.
- `set_font(font)`: alias for `font(font)`.
- `pixel(x, y)`: draw one pixel.
- `line(x0, y0, x1, y1)`: draw a line.
- `rect(x, y, width, height)`: draw a rectangle outline.
- `fill_rect(x, y, width, height)`: draw a filled rectangle.
- `circle(x, y, radius)`: draw a circle outline.
- `fill_circle(x, y, radius)`: draw a filled circle.
- `text(x, baseline_y, text)`: draw UTF-8 text.
- `refresh()`: present the graphics buffer.
- `present()`: alias for `refresh()`.
- `getch([timeout_ms])`: return a key code or `None`.

Example:

```python
import solaros
from solaros import gfx

gfx.begin()
w, h = gfx.size()
gfx.clear(gfx.WHITE)
gfx.color(gfx.BLACK)
gfx.rect(8, 8, w - 16, h - 16)
gfx.font(gfx.FONT_BOLD)
gfx.text(24, 36, "SolarOS Graphics")
gfx.color(gfx.gray(12))
gfx.fill_circle(w // 2, h // 2, 36)
gfx.color(gfx.BLACK)
gfx.circle(w // 2, h // 2, 36)
gfx.refresh()

while not solaros.should_exit():
    key = gfx.getch(250)
    if key == gfx.KEY_ESCAPE:
        break

gfx.end()
```

For an attached auxiliary display, use the target name:

```python
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx.end()
```

## Longer Example: Status Snapshot

```python
import solaros

solaros.write("SolarOS {}\n".format(solaros.version()))
solaros.write("{}\n".format(solaros.identity.format()))
solaros.write("uptime {}\n".format(solaros.time.uptime()))

battery = solaros.battery.status()
solaros.write("battery {}% {} mV\n".format(battery["percent"], battery["voltage_mv"]))

env = solaros.sensors.environment()
solaros.write("env {:.1f} C {:.1f}%\n".format(env["temperature_c"], env["humidity_percent"]))

wifi = solaros.wifi.status()
solaros.write("wifi {} {}\n".format(wifi["state"], wifi["ip"]))
```

## Not Exposed Yet

The Python bridge intentionally does not expose raw SSH/SCP session handles yet. Those APIs need object lifetime, ownership, and event-loop rules before they can safely become scriptable.
