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
| [Networking](python.network.md) | `solaros.wifi`, `solaros.mqtt`, `solaros.http`, `solaros.net`, `solaros.ftp`, `solaros.sftpsync`, `solaros.ssh_keys` |
| [Bluetooth](python.ble.md) | `solaros.ble` |
| [GPIO and peripherals](python.hardware.md) | `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.servo`, `solaros.motor`, `solaros.encoder`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.sensors`, `solaros.gnss`, `solaros.nfc` |
| [Buses and expansion](python.buses.md) | `solaros.buses`, `solaros.expansion` |
| [Audio and control](python.audio.md) | `solaros.audio`, `solaros.speech`, `solaros.synth`, `solaros.dsp`, `solaros.controls`, `solaros.parameters`, `solaros.midi`, `solaros.osc` |
| [Emotion engine](emotion.md) | `solaros.emotion` facial animation and reaction sounds |
| [Jet 3D renderer](jet.md) | `solaros.jet` scenes, meshes, lighting and presentation |
| [Input and clipboard](python.input.md) | `solaros.input`, `solaros.hid`, `solaros.clipboard` |
| [Apps, jobs, and identity](python.system.md) | `solaros.identity`, `solaros.jobs`, `solaros.sessions`, `solaros.apps` |
| [Contacts and messages](python.messaging.md) | `solaros.contacts`, `solaros.messages` |
| [SC2 end-to-end chat security](python.sc2.md) | `solaros.sc2` |
| [Text user interfaces](python.tui.md) | `solaros.tui` |
| [Graphics, raster images, and DLP connections](python.gfx.md) | `solaros.gfx`, `solaros.image`, `solaros.dlp` |
| [Camera, streams, and RTSP](python.media.md) | `solaros.streams`, `solaros.camera`, `solaros.rtsp` |
| [Computer vision](vision.md) | `solaros.vision` |
| [Model inference](inference.md) | `solaros.inference` |
| [Native image pipelines](pipelines.md) | `solaros.pipeline` |

## Conventions

Most mutating functions return `None` on success and raise `OSError("ESP_ERR_...")` on service failure. Query functions return strings, integers, booleans, dictionaries, or lists.

Supported language features include f-strings, sets, properties, descriptors,
`enumerate()`, `filter()`, `reversed()`, `memoryview`, and `frozenset`. The
importable runtime modules are `array`, `binascii`, `cmath`, `collections`,
`errno`, `gc`, `hashlib`, `io`, `json`, `math`, `micropython`, `random`,
`struct`, and `sys`.

Python integers support the signed 64-bit range. SolarOS service values such as
file sizes, timestamps, counters, and storage capacities use integers throughout
that range. An unsigned native value above `9223372036854775807` is returned as
a decimal string instead of losing precision.

`input()`, `execfile()`, and upstream `extmod` modules outside this selected
set remain disabled. Use the typed `solaros` service APIs instead.

The selected modules include `json.loads()` and `json.dumps()`, hexadecimal and
Base64 conversions in `binascii`, SHA-256 in `hashlib`, and the usual
non-cryptographic `random` helpers. Use `hashlib` for hashing; `random` is not suitable for security-sensitive
values.

Functions that accept file paths use SolarOS shell-style paths. `/` means the default storage mount; internally this resolves to the active storage mount point.

## Service availability

Python requires PSRAM. Optional `solaros` modules appear only when their
services are included in the firmware. Check `pkg` and `board` on your device;
the topic table above links to each service's requirements and examples.
For an optional module, test availability before using it:

```python
import solaros

if hasattr(solaros, "wifi"):
    print(solaros.wifi.status())
```

Vision, inference, and pipelines need their own packages; adding Python alone
does not enable them. See [Packages and flavors](packages.md).

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

Raw SSH/SCP session handles are unavailable in scripts. Use the `ssh` and
`scp` applications for remote access and file transfers.

## Quick reference

Import `solaros` and use its service tables for storage, time, networking,
hardware, jobs, sessions, input, TUI, and graphics. Foreground pointer and axis
events use solaros.input sources, read, clear, and status; keyboard characters
use solaros.tui.getch(). APIs return `None` or raise `OSError` as documented.
Long-running programs must yield cooperatively and release opened buses,
graphics targets, and other resources in `finally`.
