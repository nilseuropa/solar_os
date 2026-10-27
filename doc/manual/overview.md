+++
id = "overview"
title = "SolarOS overview"
section = "concept"
summary = "Understand boards, drivers, services, displays, apps, jobs, and parameters"
aliases = ["solaros", "manual"]
keywords = "solaros architecture runtime drivers services boards capabilities flavors packages modules shell sessions apps jobs displays ports buses resources streams parameters controls schedules python lua"
packages_any = []
+++
# SolarOS overview

SolarOS turns an ESP32 board into a pocket computer or a small terminal. Use
the shell to manage files and hardware, open apps, run background jobs, or
write Python and Lua programs. A board can have its own screen and keyboard,
or you can use it through a serial or network terminal.

## How the parts fit together

| Layer | Role | Example |
| --- | --- | --- |
| Board profile | Describes the hardware, pins, and built-in devices. | A screen, SD slot, and keyboard. |
| Drivers | Communicate with particular hardware. | A display controller or temperature sensor. |
| Services | Give programs a shared way to use hardware and OS features. | Storage, audio, time, input, and networking. |
| Commands, apps, jobs, and scripts | Use services to do useful work. | Read a file, play music, or record sensor data. |

For example, a sensor driver reads a chip, the sensor service exposes its
temperature, and a command or script reads that value. Programs can use the
same service with built-in hardware or an attached expansion device.

## Boards, capabilities, and packages

A **board profile** describes a supported hardware target. **Capabilities**
describe what it can support, such as graphics, Wi-Fi, or expansion I2C.
A **firmware flavor** selects a set of compiled **packages**: services,
drivers, apps, and jobs. Features also depend on the hardware being present.

Check your device before following an example:

```text
board
pkg system
apps
jobs
```

[Boards](boards.md) lists targets and wiring. [Packages and flavors](packages.md)
explains how to choose or build an image.

[SolarWriter](boards.md#solarwriter) combines the CrowPanel 5.79-inch wide
e-paper display with INMP441 capture, shared PCM5102/MAX98357 playback, and
an LC709203F battery gauge. Its fixed attachments use the same reusable
expansion drivers available on other compatible boards.

## Reusable runtime drivers and buses

A **bus** is a named connection such as `i2c0` or `spi0`. A **driver** knows
how to use a particular device on that connection. An **attachment** is a
configured instance of the driver, with a device name and wiring.

Built-in devices attach at boot and are fixed. Where the board and firmware
allow it, you can attach more devices at runtime using the same drivers.
Attaching hardware publishes it to the relevant services: an added display
becomes a display target, for example.

```text
expansion bus list
expansion drivers
expansion devices
```

Pins, buses, ports, and devices can have an **owner** while in use. If a
resource is busy, inspect its owner and stop that user before reusing it.
See [Named buses](buses.md) and [Expansion devices](expansion.md).

## Shells, apps, sessions, and jobs

- A **shell** accepts commands on a display or terminal connection. Paths,
  history, and typed input belong to that shell.
- An **app** is an interactive foreground program, such as Files or Reader.
  Start it by name, for example `files`.
- A **session** holds a shell or app on a particular display or port.
  Resumable apps keep their state when you switch away.
- A **job** works in the background, such as a logger or HTTP server. It
  continues when you switch apps. Start and stop jobs explicitly.

```text
session list
job start batmon 60
job status batmon
job stop batmon
```

On a display, `Alt+Tab` switches sessions and `Ctrl+Alt+Del` closes the app.
On a port terminal, `Ctrl+Z` suspends a resumable app, `fg` resumes it, and
`Ctrl+]` closes it. See [Sessions](sessions.apps.md), [Apps](apps.md), and
[Jobs](jobs.md).

## Displays, terminals, and input

A **display target** is a named screen or canvas. It can be a physical panel,
joined panels, a split-screen viewport, or a virtual display. Each target
has its own active session. A **port** carries bytes, for example over UART,
USB CDC, or a Link radio connection; port shells use terminal text.

**Local input focus** selects which display receives keyboard and board-control
input. Changing focus does not move an app. Touch and browser input can target
a specific display. **TUI** apps use text menus; **GUI** apps use graphics.
Check an app's requirements before launching it from a port shell.

```text
display list
port list
session focus
```

See [Navigation and keys](navigation.md) for common shortcuts and target
control mappings. See [Display commands](commands.md#hardware-and-time), [Sessions](sessions.apps.md), and
[Input and audio](media.input.md).

## Streams, parameters, and controls

A **stream** is a named source or destination for data. Scalar streams carry
measurements such as millivolts; other streams carry bytes, audio, or video.
Use `stream list` to discover them. Apps and jobs can plot, record, or process
the types they support.

A **parameter** is a named setting published by a running native app, such as
`synth.filter.cutoff`. A **control** maps an input stream or script value to
an app parameter or MIDI controller. For example, a potentiometer can adjust
Synth's filter through the `controls` job.

```text
control parameters
control list
control bindings
```

See [Continuous controls](controls.md) for a complete example. [Image pipelines](pipelines.md)
process camera or RTSP streams in the background and retain their latest result.

## Files, settings, and automation

Shell paths use `/` for the default storage volume. Other mounted volumes can
include `/flash` and `/sdcard`. Use `disk status` and `df` to check storage.
Files hold documents, scripts, captures, and installed apps.

Many system settings are saved across reboots in **NVS**, the device's settings
store. Runtime buses, attachments, control mappings, and job starts can be
restored with the startup script. Run `setterm startup` to find its path.
**Schedules** run actions at a time or interval; they do not launch foreground apps.

Python and Lua use the `solaros` APIs to access these services. **Playground**
installs script apps. On supported ESP32-S3 boards with PSRAM, **native modules**
add compiled apps, jobs, or drivers. Install only code you trust.

See [Storage](storage.md), [Scripting](script.conventions.md),
[Playground](playground.md), [Native apps](native-app.md), and
[Scheduling](commands.md#power-rtc-schedules-and-terminal-settings).

## Connections, power, and updates

The network service manages connections and routes; `network status` shows
the current path. `power status` shows the power profile and sleep settings.
Suspending the display keeps jobs running; deep sleep starts a fresh boot
on wake. Firmware updates use `ota` on boards with an OTA layout.
See [Networking](network.md) and [Power and updates](commands.md).

## Finding documentation

```text
help
man -k connect wifi
man sessions.apps
```

The website has the full manual. Device firmware includes setup help and a
topic directory. With Wi-Fi, PSRAM, and SD storage, `help update` installs the
manual for your firmware version; it then works offline while the card is
mounted. See [Documentation setup](help.md).

## Quick reference

Board profiles describe hardware; capabilities and firmware flavors determine
which packages are available. Drivers attach devices to named buses; services
expose them to commands, apps, jobs, and Python/Lua. Apps run in foreground
sessions on displays or ports; jobs run in the background. Local input focus
selects a display. Streams carry data; controls map values to app parameters or
MIDI. Files hold user data, NVS holds persistent settings, and startup scripts
restore runtime configuration. Discover names with board, pkg, apps, jobs,
display list, port list, expansion status, and stream list. Use help or man to
find usage; help update installs the full manual on supported devices.
