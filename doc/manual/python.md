+++
id = "python"
title = "Python API overview"
section = "api"
summary = "Runtime basics, conventions, and service API topic index"
aliases = ["micropython", "python.api"]
keywords = "python micropython solaros api storage wifi gpio buses gfx tui examples"
packages_any = ["app_python"]
agent_reference_sections = true
+++
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

## API topics

Open a topic below, or use its ID with `man` on the device, for example
`man python.network`. Service availability depends on the board and flavor.

| Topic | Services |
| --- | --- |
| [Storage and files](python.storage.md) | `solaros.storage` |
| [Time and scheduling](python.time.md) | `solaros.time`, `solaros.rtc`, `solaros.schedule` |
| [Networking](python.network.md) | `solaros.wifi`, `solaros.mqtt`, `solaros.http`, `solaros.net`, `solaros.ftp`, `solaros.ssh_keys` |
| [Bluetooth](python.ble.md) | `solaros.ble` |
| [GPIO and peripherals](python.hardware.md) | `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.sensors`, `solaros.gnss`, `solaros.nfc` |
| [Buses and expansion](python.buses.md) | `solaros.buses`, `solaros.expansion` |
| [Audio and control](python.audio.md) | `solaros.audio`, `solaros.synth`, `solaros.dsp`, `solaros.controls`, `solaros.parameters`, `solaros.midi`, `solaros.osc` |
| [Input and clipboard](python.input.md) | `solaros.input`, `solaros.hid`, `solaros.clipboard` |
| [Apps, jobs, and identity](python.system.md) | `solaros.identity`, `solaros.jobs`, `solaros.sessions`, `solaros.apps` |
| [Contacts and messages](python.messaging.md) | `solaros.contacts`, `solaros.messages` |
| [Text user interfaces](python.tui.md) | `solaros.tui` |
| [Graphics](python.gfx.md) | `solaros.gfx` |

## Conventions

Most mutating functions return `None` on success and raise `OSError("ESP_ERR_...")` on service failure. Query functions return strings, integers, booleans, dictionaries, or lists.

SolarOS uses MicroPython's size-conscious `EXTRA` language profile. This adds
common language features such as f-strings, sets, properties, descriptors,
`enumerate()`, `filter()`, `reversed()`, `memoryview`, and `frozenset`. The
importable runtime modules are `array`, `binascii`, `cmath`, `collections`,
`errno`, `gc`, `hashlib`, `io`, `json`, `math`, `micropython`, `random`,
`struct`, and `sys`.

`input()`, `execfile()`, and upstream `extmod` modules outside this selected
set remain disabled. Use the typed `solaros` service APIs instead.

The selected modules include `json.loads()` and `json.dumps()`, hexadecimal and
Base64 conversions in `binascii`, SHA-256 in `hashlib`, and the usual
non-cryptographic `random` helpers. SolarOS seeds `random` from the ESP32
hardware random source when the module is first imported. Use `hashlib` for
hashing and an appropriate SolarOS security service, not `random`, for
security-sensitive values.

Functions that accept file paths use SolarOS shell-style paths. `/` means the default storage mount; internally this resolves to the active storage mount point.

## Service availability

The Python runtime package requires PSRAM. Hardware and network helpers are
added only when the board/flavor includes their service package. For example,
an ODROID-GO full build includes Python with `solaros.spi` and
`solaros.onewire`, while omitting `solaros.adc` and `solaros.i2c` because those
service packages are not available on that board.

Optional API groups follow these package gates:

- `service.wifi`: top-level `wifi_status` and `solaros.wifi`
- `network.mqtt`: `solaros.mqtt`
- `network.http-client`: `solaros.http`
- `network.ftp`: `solaros.ftp`
- `network.base`: `solaros.net`
- `network.ssh`: `solaros.ssh_keys`
- `service.ble`: `solaros.ble`
- `service.hid`: `solaros.hid`
- `service.gpio`: `solaros.gpio` and `solaros.led`
- `service.onewire`: `solaros.onewire`
- `service.messaging`: `solaros.contacts` and `solaros.messages`
- `service.adc`, `service.pwm`, `service.i2c`, `service.spi`, and
  `service.uart`: their matching submodules
- `service.audio`, `service.synth`, `service.battery`, and `service.sensors`:
  their matching helpers and submodules
- `service.gnss` and `service.nfc`: `solaros.gnss` and `solaros.nfc`
- `service.dsp`: `solaros.dsp` fixed-point block operations and caller-owned
  FIR, decimator, and FFT processors

## Top-Level Helpers

- `solaros.write(text)`: write text to the SolarOS terminal.
- `solaros.version()`: return the SolarOS firmware version string.
- `solaros.should_exit()`: return `True` when the app is being asked to stop.
- `solaros.tick_interval([ms])`: get or set the foreground event-pump interval in milliseconds. Pass `0` to restore the 25 ms default.
- `solaros.battery_status()`: shortcut for `solaros.battery.status()` when battery support is compiled.
- `solaros.wifi_status()`: compact Wi-Fi status shortcut when Wi-Fi support is compiled.
- `solaros.environment()`: shortcut for `solaros.sensors.environment()` when environmental sensor support is compiled.

For example, `solaros.tick_interval(5)` lets a foreground Python app drain
terminal, TUI, and graphics events at a best-effort 5 ms cadence. It does not
schedule or preempt Python code, and it is not a hard-real-time timer. The
setting lasts for the current foreground Python app only; headless script jobs
cannot change it.

## Not Exposed Yet

The Python bridge intentionally does not expose raw SSH/SCP session handles yet. Those APIs need object lifetime, ownership, and event-loop rules before they can safely become scriptable.

## Quick reference

Import `solaros` and use its service tables for storage, time, networking,
hardware, jobs, sessions, input, TUI, and graphics. Foreground pointer and axis
events use solaros.input sources, read, clear, and status; keyboard characters
use solaros.tui.getch(). APIs return `None` or raise `OSError` as documented.
Long-running programs must yield cooperatively and release opened buses,
graphics targets, and other resources in `finally`.
