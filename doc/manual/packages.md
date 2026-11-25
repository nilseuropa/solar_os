+++
id = "packages"
title = "Firmware packages and flavors"
section = "build"
summary = "Choose firmware features and create custom builds"
aliases = ["flavors"]
keywords = "packages flavors groups capabilities build custom firmware pkg"
packages_any = []
+++
# Firmware Packages and Flavors

A **flavor** selects the apps, jobs, engines and expansion drivers compiled
into a firmware image. OS services are part of the core in every flavor.
A **group** is a convenient selection, such as `scripting` or `vision`.
A **package** supplies a particular app, job, service, or driver.
Required dependencies are included automatically; unsupported packages are
removed for the selected board.

The core includes `service.usb-host`, `service.usb-storage` and
`service.usb-serial` on boards with the `USB_HOST` capability. This provides
USB input, CDC-ACM/CP210x/CH340 serial ports, and one FAT USB flash or UFI/CBI
floppy drive at `/usb0`. Hosting
starts disabled; arrange host wiring and external 5 V peripheral power, then
run `usb mode host`. See [USB host](usb.host.md).

## Inspect the running image

```text
pkg system
board
apps
jobs
```

`pkg system` shows compiled packages. `pkg installed` lists native modules
and identifies native components included in firmware. A compiled driver is available to use, but does not mean its
hardware is attached; check `expansion devices`.

Use `pkg update` to refresh the signed native-module catalog and list installed
modules available for upgrade. `pkg upgrade` shows and confirms the upgrade
plan; `pkg upgrade ID` selects one installed module and its dependencies.
See [Native ELF modules](native-modules.md) for version tracking, compatibility
checks, and preparation of active modules.

Native packages can require exact versions of library packages and minimum
firmware service tables. `pkg install cc` installs its matching `tinycc` library
first. Installation verifies the complete dependency graph and required services
before downloading artifacts. A dependency already on storage is reused only
when its size and SHA-256 match the signed catalog. A matching compiled
component also satisfies an exact dependency. Close or unload active modules
before replacing them.

For ESP32-S3 boards with PSRAM, the optional `cc` firmware group includes the C
command and TinyCC provider in the image. The `full` flavor enables it by
default on supported boards. Other flavors can enable `cc = true` under
`[groups]`; set it to `false` in a custom flavor to omit it. The built-in command
needs no installed compiler ELFs and supports the same projects, SDK headers and
limits as the external packages. Code stays in firmware flash; private compiler
state is placed in PSRAM when external BSS is enabled. The compiler heap and
joined worker stack are allocated on demand. The built-in provider takes
precedence over an installed `tinycc` library; leases protect it until clients
finish, and unloading releases its runtime snapshot without removing firmware
code. See [C development](c.md).

Python and Lua are also compiled into the firmware when their `python` and
`lua` groups are selected. The `full` flavor enables both on boards with PSRAM;
custom flavors can omit either runtime independently. Inspect `pkg system`
for `app.python`, `app.lua`, and `app.cc` in the running image.

Each artifact is validated and replaced separately. If a later download fails,
successfully installed dependencies remain available and the previous requested
module is preserved. Removing a library can leave its consumers unable to run;
reinstall the consumer to restore its declared dependencies.

## Choose features

Use the [SolarOS Builder](#solaros-builder) to select a board and flavor, or
create a custom flavor below. Services are automatic when supported. These groups select additional apps,
workers, engines and drivers:

| Group | Use | Guide |
| --- | --- | --- |
| `cc` | Compile native C applications with the built-in command and TinyCC provider on ESP32-S3 boards with PSRAM. | [C development](c.md) |
| `inference_backend` | Include the ESP-DL engine in firmware; otherwise install the `espdl` library. | [Model inference](inference.md) |
| `zoo` | Browse and install model bundles. | [Zoo](apps.md#zoo) |
| `pipelines` | Include the model-upload worker for native image pipelines. Also select the desired processor. | [Image pipelines](pipelines.md) |
| `pwm` | Use native PWM outputs where supported. | [GPIO and PWM](gpio.analog.md) |
| `pca9685` | Add 16 PWM outputs over expansion I2C. | [PCA9685](servo.md#pca9685-expansion-controller) |
| `drv8830` | Control brushed DC motors over named I2C buses. | [Motor control](motor.md) |
| `pcnt_encoder` | Attach native A/B encoders with explicit x1/x2/x4 counting. | [Encoder counts](encoder.md) |
| `ds3231` | Add a battery-backed I2C clock and hardware alarm. | [DS3231](expansion.md#ds3231-real-time-clock) |

Media, vision and inference services are available when the target and hardware
support them. Selecting Python or Lua does not change that service set. Engines
such as ESP-DL can remain external libraries. Stored-image processing needs no
camera or display. Audio can use an expansion output on a board without a
built-in speaker.

For external hardware, select its driver group and check the board's expansion
capabilities and available pins. Built-in hardware uses fixed instances of the
same drivers. See [Expansion](expansion.md) for attachment commands and
[Boards](boards.md) for hardware limits.

## Custom build rules

Flavor files normally select groups in `packages/solar_os_packages.toml`.
Dependencies and board requirements are resolved automatically:

- A group selects a feature and its required packages. Explicitly disabling
  a required dependency is an error.
- The board's required drivers are always included. Unsupported features and
  their dependants are removed for the selected board and MCU.
- Core services and `bootstrap` are automatic and cannot be disabled by a
  flavor. Target, hardware and update-layout limits still apply.
- Categories organize the builder; select individual groups within them.
- Emulators such as Game Boy, Apple II and Commodore 64 are installed as native
  modules rather than selected as firmware groups. Check `pkg available` for
  compatible packages. See [Native ELF modules](native-modules.md) and
  [Native foreground apps](native-app.md).

The `qemu_rgb`, `qemu_eth`, `qemu_input`, `qemu_storage` and `qemu_audio` groups
provide virtual display, network, input, host-directory storage and playback
devices. They are restricted to QEMU targets; the board profiles select their
required devices automatically. Host-side options in `platformio.ini` control
the shared directory, audio backend and pointer mode. See
[Running SolarOS in QEMU](qemu.md).

Package source ownership and service internals are documented in the
[maintainer package reference](https://github.com/nilseuropa/solar_os/blob/main/doc/package-architecture.md).

## Firmware profiles

SolarOS provides four firmware profiles. `core` supplies the services, shell,
package installer on supported targets, and required board hardware. It selects
no additional apps, workers or expansion-driver collections. `full` adds the
broad application and driver selection. `classic` is a compact
profile with ADC D-pad support for ODROID-GO, the I/O app, control bindings,
and 1-Wire. It is the default for all classic ESP32 targets, including boards
with 4 MiB flash.
`writerdeck` is the compact profile used by e-paper boards with smaller
application partitions.

`classic` retains Python, Playground, Editor, Pager, Files, Launcher, the I/O
app, the serial terminal, Clock, Calculator and file-transfer clients. It keeps
every eligible core service and the selected board's required hardware drivers.
Reader, Writer, Notes, media and web apps, Agent, Chat, the legacy chat workers,
network server workers, and the speech worker are omitted to fit 4 MiB boards.
Lua, built-in games, video playback and pipeline workers are also omitted. ODROID-GO retains its buttons and
ADC D-pad; VGA32 retains its default PS/2 keyboard attachment.

`writerdeck` includes Python, Playground, editing, Files, Reader, Writer, Notes,
View, audio playback, recording, Web, Agent, WireGuard, SC2 messaging, and file
transfer. The synthesizer app and controls worker are omitted;
the Synth service remains available.
Its transfer tools include FTP/FTPD, SSH, SCP, SFTP, SFTP Sync, and Curl.
PCM5102/I2S and PWM audio remain available for runtime expansion outputs.
The screen keyboard, Bridge, DAQ, IRC, legacy gateway client/server, speech
synthesis, broad expansion-driver collections, external radio stacks, Lua,
built-in games, video playback, and RTSP are omitted. Board and target
requirements still prune unsupported features from every profile.

On ESP32-S3 boards with PSRAM, all profiles include `pkg` for installing
native modules. Native-module installation is unavailable on classic ESP32;
Python and Playground remain available on its PSRAM boards. On supported
targets, all profiles use the downloadable `espdl` library for inference.
Custom profiles can select `inference_backend` to include the ESP-DL engine
in firmware.

## Application requirements

Check these requirements when selecting a custom flavor. Built-in audio is
not required when a compatible runtime-attached device provides the input or
output.

| Feature or group | Requirements and behavior |
| --- | --- |
| `launcher` | Graphics. |
| `image_viewer` | Graphics and PSRAM for decoded images. |
| `photo` | Live JPEG viewfinder and still capture; graphics and PSRAM, with RTSP reception on Wi-Fi boards. |
| `sketch` | Graphics; accepts absolute or relative pointers when available. |
| `player` | A playback output for local WAV/MP3 files; supports saved playlists. |
| `webradio` | Wi-Fi and a playback output for HTTP(S) MP3 streams. |
| `recorder` | A capture stream; monitoring also needs a playback output. |
| `funcgen` | A playback output; includes the Synth service. |
| `agent` | Wi-Fi and PSRAM. Add Python or Lua separately to run scripts. See [Agent](agent.md). |
| `synth` | A playback output; needed by native apps that use synthesized audio. |
| `inmp441` | Includes the INMP441 and ICS-43432 I2S microphone drivers. See [I2S microphones](expansion.reference.md#inmp441-and-ics-43432-i2s-microphones). |

## SolarOS Builder

Use the host-side `os_builder` to create or modify a selection, build it, and
flash it without editing TOML by hand:

```sh
python3 scripts/os_builder.py
```

The builder starts with board selection and then asks for the update layout.
The board establishes the MCU target, capabilities, flash and PSRAM limits, and
the drivers that must be included. The layout establishes the application-image
limit: 8 MiB and 16 MiB targets can keep two images for OTA updates or use one
larger image for serial updates. A 4 MiB target has one fixed serial-update
layout. Internal filesystem size follows the layout and is not a user setting.
Flavors remain portable because neither the board nor the layout is stored in
the flavor TOML.

The main screen contains selectable groups under collapsible category headings.
The same group names are used in the TOML file. Categories are headings only,
and services and internal packages are never listed as choices. The resolver
adds dependencies automatically. `[!]` marks a group required by the selected
board, `[+]` marks a group whose feature is already present through another
selection, and `[-]` marks a group that the board cannot support.

Each group shows its marginal estimated contribution to the current image.
The detail line separates the group's own contribution from additional
automatic support. Shared objects and components are counted once, so the
number can change with the surrounding selections. Before the first build, the
top bar compares the estimated image with the selected layout's image limit.

When available, estimates use text and initialized-data sizes from cached
PlatformIO objects and the ESP-IDF component archives declared by package
requirements. Otherwise, the builder identifies that it is using a source-size
fallback. A specific board, layout, and cached build can be selected from the
command line:

```sh
python3 scripts/os_builder.py \
  --input flavors/writerdeck.toml \
  --output flavors/my-flavor.toml \
  --board solar_term \
  --layout ota
```

Press `b` to build the current selection. The builder writes a private working
flavor under `.pio/os_builder/`; it does not overwrite the selected input or
the output flavor. During the build it shows a progress bar and the current
compiler stage. A successful build replaces the total estimate with the exact
`firmware.bin` size and recalibrates marginal group estimates from that board's
artifacts. Change groups and press `b` again to iterate.

Press `f` to flash. Flashing is enabled only after a successful build whose
board, layout, and groups still match the current screen. Any selection change
makes the build stale until it is rebuilt. The upload has its own progress bar
and confirmation; `--upload-port` can select a serial device explicitly.
Build and flash failures open a concise error view, with `a` toggling the full
output. Complete logs are replaced on each operation at
`.pio/os_builder/build.log` and `.pio/os_builder/flash.log`. Press `s` whenever
you want to save the portable group selection as TOML; saving does not end the
build/flash loop.

## Custom Flavor Example

This flavor selects only the user-facing HTTP client group. The generator adds
the internal HTTP transport and other dependencies automatically:

```toml
[flavor]
name = "curl-only"
description = "Bootstrap plus the HTTP client app."

[groups]
http_client = true
```

An optional `[packages]` table can override
individual internal packages. The builder does not expose that table as
another selection layer.

Use `pkg system` on the device to inspect the resolved firmware package list.
Use `pkg` without arguments to browse signed native modules in the package
manager.

## Quick reference

Select a board, an update layout, and then granular groups. Build to replace the
total estimate with a measured image size; adjust, rebuild, and flash when it
fits. A flavor stores only portable groups. The board supplies required hardware
drivers and removes unsupported groups, while the layout defines the image
limit. Internal packages and services are resolved automatically. Use
`pkg system` on-device to inspect the resolved firmware.
