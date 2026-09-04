+++
id = "overview"
title = "SolarOS manual"
section = "concept"
summary = "Find commands, applications, jobs, scripting APIs, and hardware concepts"
aliases = ["solaros", "manual"]
keywords = "solaros api modules python lua help reference capabilities commands apps jobs"
packages_any = []
+++
# SolarOS manual

SolarOS is a small ESP32 operating environment for pocket terminals, reflective
displays, serial consoles, and low-power embedded tools. It is text-first, but
not text-only: the core experience is a shell with local storage, sessions,
jobs, device services, networking, scripting, and foreground applications.

On display boards SolarOS behaves like a self-contained handheld terminal. On
headless boards it runs through UART or USB CDC as a compact networked
maintenance node. The primary target is the Waveshare
ESP32-S3-RLCD-4.2, but the codebase is built around board capabilities rather
than one fixed product shape.

## What SolarOS can do

- Run a local shell with history, aliases, scripts, tab completion, storage,
  and resumable sessions.
- Launch foreground applications such as an editor, pager, file manager,
  reader, image viewer, serial terminal, SSH/Telnet clients, web client,
  plotter, clock, chat client, games, Python, and Lua.
- Keep background jobs running for logging, data acquisition, NTP sync, SLIP,
  HTTP serving, Telnet shell access, serial bridging, battery monitoring, and
  gateway messaging synchronization.
- Use Wi-Fi, BLE, USB CDC, UART, SD or flash storage, RTC time, GPIO, ADC, PWM,
  I2C, SPI, 1-Wire, audio, sensors, and board-specific display hardware through
  shared SolarOS services.
- Capture streams to CSV or raw files, transfer files over byte-stream ports,
  capture GPIO waveforms through SUMP or the on-device logic analyzer, and
  inspect runtime resource ownership.
- Carry ordinary SolarOS port shells and serial bridges over peer-bound,
  retransmitted Link virtual serial ports on packet radios.
- Build focused or full firmware images through capability-aware package
  flavors.

The result is a deliberately small runtime for turning inexpensive
microcontroller hardware into useful field terminals, diagnostic tools,
portable loggers, serial/network bridges, and scripting surfaces.

## Runtime model

- Shells are interactive command surfaces on displays, UART, USB CDC, Telnet,
  or SSH.
- Applications are foreground programs. They may be text, graphics,
  display-only, or port-capable according to their registry flags.
- Jobs are background workers with explicit resource and memory claims, so
  ports, files, streams, and listeners have visible owners.
- Services provide shared storage, terminal, session, port, network, time,
  sensor, hardware I/O, graphics, scripting, OTA, and power behavior.
- Board profiles describe capabilities and pins. Runtime code asks for those
  capabilities instead of assuming a particular display, storage bus, or
  peripheral layout.

Drivers own hardware detail, services own policy, and applications, jobs, and
shell commands use services.

## Hardware targets

Built-in targets include:

- `solar_term`: SolarTerm pocket terminal built around the Waveshare
  ESP32-S3-RLCD-4.2 board.
- `freenove_esp32_s3_display_4_0`: integrated 480x320 capacitive-touch terminal
  with speaker, microphone, SD, and battery monitoring.
- `elecrow_crowpanel_esp32_s3_4_2_epaper`: 400x300 e-paper HMI with rotary
  controls and microSD.
- `odroid_go`: classic ESP32 handheld.
- `ttgo_vga32_v14`: classic ESP32 desktop terminal with VGA, mono DAC audio,
  PS/2 keyboard, and microSD.
- `esp32_s3_devkitc1_n16r8`: minimal headless ESP32-S3 target.

See `man boards` for the complete board table, capability flags, pins, build
environments, and bring-up checklist. See `man packages` for firmware flavors
and package ownership.

## First commands

Once SolarOS boots, these commands give a quick view of the installed system:

```text
help
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
was compiled and which hardware is exposed.

## Finding documentation

Optional topics appear only when their package is part of the firmware. Open
`help` for the foldable manual tree, or search by task:

```text
man --list
man -k draw a circle
man -k connect wifi
man -k background job memory
```

Open a result with `man TOPIC`. Graphic display shells can also open topics from
`help` in `reader`; text shells use `less`. Arrow and Page Up/Page Down keys
scroll, `/` searches inside a page, and the app-exit key returns to the shell.

## Where to begin

- Use `man scripting` before writing a Python or Lua application.
- Use `man python.gfx` or `man lua.gfx` for a graphical application.
- Use `man jobs` to understand background workers and their memory.
- Use `man buses` and `man expansion` before connecting external hardware.
- Use `man identity` to configure the device user and hostname.
- Use `man help` to learn how to refresh the signed manual on a supported device.

## Quick reference

Search again with a module or task name. Topics cover gfx, tui, storage,
identity, commands, jobs, sessions, applications, boards, packages, Wi-Fi,
MQTT, networking, GPIO, ADC, PWM, buses, I2C, SPI, UART, OneWire, expansion,
audio, BLE, clipboard, time, battery, sensors, Python, Lua, and SSH keys.
Optional modules exist only when their package is installed.
