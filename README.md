# SolarOS

SolarOS is a small ESP32 operating environment for pocket terminals, reflective
displays, serial consoles, and low-power embedded tools.

It is text-first, but not text-only. The core experience is a shell with local
storage, sessions, jobs, device services, networking, scripting, and foreground
apps. On display boards it behaves like a self-contained handheld terminal. On
headless boards it can run through UART or USB CDC as a compact networked
maintenance node.

The primary target is the Waveshare ESP32-S3-RLCD-4.2, but the codebase is built
around board capabilities rather than one fixed product shape.

## What SolarOS Can Do

SolarOS turns an ESP32 board into a standalone system for small real-world
workflows:

- Run a local shell with history, aliases, scripts, tab completion, storage, and
  resumable sessions.
- Launch foreground apps such as an editor, pager, file manager, reader, image
  viewer, serial terminal, SSH/Telnet clients, web client, plotter, clock,
  chat client, games, Python, and Lua.
- Keep background jobs running for logging, data acquisition, NTP sync, SLIP,
  HTTP serving, serial bridging, battery monitoring, and chat gateway service.
- Use Wi-Fi, BLE, USB CDC, UART, SD or flash storage, RTC time, GPIO, ADC, PWM,
  I2C, SPI, 1-Wire, audio, sensors, and board-specific display hardware through
  shared SolarOS services.
- Capture data streams to CSV or raw files, transfer files over byte-stream
  ports, capture GPIO waveforms through SUMP or the on-device logic analyzer,
  and inspect runtime resource ownership.
- Build smaller or larger firmware images through package flavors instead of
  treating every app and service as mandatory.

The result is a deliberately small runtime for
turning inexpensive microcontroller hardware into useful field terminals,
diagnostic tools, portable loggers, serial/network bridges, and scripting
surfaces.

## Runtime Model

SolarOS is organized around a few stable roles:

- Shells are interactive command surfaces. They can run on the display, UART, or
  USB CDC when the board exposes those targets.
- Apps are foreground programs. They can be text, graphics, display-only, or
  port-capable depending on their registry flags.
- Jobs are background workers with explicit resource claims, so ports, files,
  streams, and listeners have visible owners.
- Services provide shared behavior for storage, terminal rendering, sessions,
  ports, networking, time, sensors, hardware I/O, graphics, scripting, OTA, and
  power policy.
- Boards describe capabilities and pins. Runtime code asks for capabilities
  instead of assuming a specific display, storage bus, or peripheral layout.

This split is what lets the same shell commands, apps, and jobs run across various
boards. The guiding rule is simple: drivers own hardware detail, services own policy,
and apps, jobs, and shell commands use services.

## Hardware Targets

Current built-in targets include:

- `waveshare_esp32_s3_rlcd_4_2`: primary reflective-display pocket terminal.
- `elecrow_crowpanel_esp32_s3_4_2_epaper`: 400x300 SSD1683 e-paper HMI with rotary controls and microSD.
- `odroid_go`: classic ESP32 handheld target.
- `esp32_s3_devkitc1_n16r8`: minimal headless ESP32-S3 target.

Board details, capability flags, pin metadata, and the board bring-up checklist
are documented in [Defining SolarOS Boards](doc/solar_os_boards.md).

## Build

SolarOS uses PlatformIO with ESP-IDF through the pioarduino Espressif32 platform.

```sh
pio run -e waveshare_esp32_s3_rlcd_4_2
pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
pio run -e odroid_go
pio run -e esp32_s3_devkitc1_n16r8
pio run -t upload
pio device monitor -b 115200
```

The default build uses the full firmware flavor. Smaller images can be built
with flavor files in `flavors/`:

```sh
SOLAR_OS_FLAVOR=core pio run -e waveshare_esp32_s3_rlcd_4_2
```

The running firmware reports its selected board, version, flavor, and package
set through the `version` and `pkg` shell commands.

## First Commands

Once SolarOS boots, these commands give a quick feel for the system:

```text
help
apps
jobs
sessions
board
pkg
wifi
stream list
python
lua
```

Availability depends on the selected board and firmware flavor. The running
device is authoritative: `help`, `apps`, `jobs`, `board`, and `pkg` show what
was compiled and what hardware is exposed.

## Documentation

Detailed documentation split by topic:

- [Shell commands](doc/commands.md)
- [Expansion ports and external devices](doc/expansion.md)
- [Foreground apps](doc/apps.md)
- [Background jobs](doc/jobs.md)
- [Service concurrency contract](doc/service-concurrency.md)
- [Board targets and board support](doc/solar_os_boards.md)
- [Python API](doc/solar_os_python.md)
- [Lua API](doc/solar_os_lua.md)
- [OTA release schema](doc/solar_os_ota_schema.md)

## Architecture
![Architecture diagram](doc/solar_os_architecture.png)

## Source Map

The source tree follows the runtime roles:

```text
src/apps/       foreground applications
src/jobs/       background job implementations
src/services/   shared OS services and runtime policy
src/shell/      shell command implementations
src/drivers/    low-level hardware drivers
boards/         board profiles and driver selection
include/boards/ board pin and capability metadata
packages/       package and flavor catalog
doc/            detailed user and developer documentation
```
