+++
id = "lua"
title = "Lua API overview"
section = "api"
summary = "Runtime basics, conventions, and service API topic index"
aliases = ["lua.api"]
keywords = "lua solaros api storage wifi gpio buses gfx tui examples"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# SolarOS Lua API

SolarOS embeds Lua as the `lua` foreground application. It can run an interactive REPL or execute `.lua` files from storage.

The SolarOS API is preloaded as the global table `solaros`. A minimal `require("solaros")` shim is also provided:

```lua
local solaros = require("solaros")

print("SolarOS " .. solaros.version())
print(solaros.identity.format())
```

Run the interpreter or a saved script from the shell:

```text
lua
lua /apps/demo.lua argument
```

Use SolarOS services for files, hardware, networking, and UI. Lua's host
`io`, `os`, and dynamic package loading are unavailable.

## API topics

Open a topic below, or use its ID with `man` on the device, for example
`man lua.network`. Service availability depends on the board and flavor.

| Topic | Services |
| --- | --- |
| [Storage and files](lua.storage.md) | `solaros.storage` |
| [Time and scheduling](lua.time.md) | `solaros.time`, `solaros.rtc`, `solaros.schedule` |
| [Networking](lua.network.md) | `solaros.wifi`, `solaros.mqtt`, `solaros.http`, `solaros.net`, `solaros.ftp`, `solaros.sftpsync`, `solaros.ssh_keys` |
| [Bluetooth](lua.ble.md) | `solaros.ble` |
| [GPIO and peripherals](lua.hardware.md) | `solaros.gpio`, `solaros.onewire`, `solaros.led`, `solaros.adc`, `solaros.pwm`, `solaros.servo`, `solaros.motor`, `solaros.encoder`, `solaros.i2c`, `solaros.spi`, `solaros.uart`, `solaros.neopixel`, `solaros.battery`, `solaros.sensors`, `solaros.gnss`, `solaros.nfc` |
| [Buses and expansion](lua.buses.md) | `solaros.buses`, `solaros.expansion` |
| [Audio and control](lua.audio.md) | `solaros.audio`, `solaros.speech`, `solaros.synth`, `solaros.dsp`, `solaros.controls`, `solaros.parameters`, `solaros.midi`, `solaros.osc` |
| [Emotion engine](emotion.md) | `solaros.emotion` facial animation and reaction sounds |
| [Jet 3D renderer](jet.md) | `solaros.jet` scenes, meshes, lighting and presentation |
| [Input and clipboard](lua.input.md) | `solaros.input`, `solaros.hid`, `solaros.clipboard` |
| [Apps, jobs, and identity](lua.system.md) | `solaros.identity`, `solaros.jobs`, `solaros.sessions`, `solaros.apps` |
| [Contacts and messages](lua.messaging.md) | `solaros.contacts`, `solaros.messages` |
| [SC2 end-to-end chat security](lua.sc2.md) | `solaros.sc2` |
| [Text user interfaces](lua.tui.md) | `solaros.tui` |
| [Graphics, raster images, and DLP connections](lua.gfx.md) | `solaros.gfx`, `solaros.image`, `solaros.dlp` |
| [Camera, streams, and RTSP](lua.media.md) | `solaros.streams`, `solaros.camera`, `solaros.rtsp` |
| [Computer vision](vision.md) | `solaros.vision` |
| [Model inference](inference.md) | `solaros.inference` |
| [Native image pipelines](pipelines.md) | `solaros.pipeline` |

## Top-Level Helpers

- `solaros.write(text)`: write to the foreground terminal.
- `solaros.version()`: return the firmware version.
- `solaros.should_exit()`: return whether the foreground app was asked to exit.
- `solaros.tick_interval([ms])`: get or set the foreground event-pump interval in milliseconds. Pass `0` to restore the 25 ms default.
- `solaros.battery_status()`: short battery status table or `nil` when battery support is compiled.
- `solaros.wifi_status()`: short Wi-Fi status table when Wi-Fi support is compiled.
- `solaros.environment()`: temperature and humidity table or `nil` when environmental sensor support is compiled.

For example, `solaros.tick_interval(5)` lets a foreground Lua app drain
terminal, TUI, and graphics events at a best-effort 5 ms cadence. It does not
schedule or preempt Lua code, and it is not a hard-real-time timer. The setting
lasts for the current foreground Lua app only; headless script jobs cannot
change it.

## Service availability

Lua requires PSRAM. Optional `solaros` tables appear only when their services
are included in the firmware. Check `pkg` and `board`, or test a table before
using it:

```lua
if solaros.wifi then
    print(solaros.wifi.status())
end
```

Vision, inference, and pipelines need their own packages; adding Lua alone
does not enable them. See [Packages and flavors](packages.md).

Lua strings can contain arbitrary bytes. Byte-oriented calls such as
`uart.read`, `i2c.read_reg`, and `mqtt.read().payload` return strings.

## Conventions

Lua tables returned as lists use normal Lua 1-based array indexes. Direct block lookup with `solaros.storage.block(index)` follows the underlying storage service index, matching Python's 0-based `block(index)`.

Raw SSH/SCP session handles are unavailable in scripts. Use the `ssh` and
`scp` applications for remote access and file transfers.

## Quick reference

Load `solaros` and use its service tables for storage, time, networking,
hardware, jobs, sessions, input, TUI, and graphics. Foreground pointer and axis
events use solaros.input sources, read, clear, and status; keyboard characters
use solaros.tui.getch(). Lua arrays are 1-based unless an individual service
explicitly exposes a native index. Close resources and keep long-running loops
cooperative.
