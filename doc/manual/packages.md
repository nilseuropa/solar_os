+++
id = "packages"
title = "Firmware packages and flavors"
section = "build"
summary = "Understand package ownership, groups, flavors, and custom builds"
aliases = ["flavors"]
keywords = "packages flavors groups capabilities build custom firmware pkg"
packages_any = []
+++
# Firmware Packages and Flavors

SolarOS package selection is declared in `packages/solar_os_packages.toml`.
Flavor files normally select granular groups. The generator translates those
groups into internal packages, resolves package dependencies, and removes
packages unsupported by the target board. A maintainer can use individual
package overrides for unusual build work, but packages are not part of the
normal flavor-configurator workflow.

`service.streams` owns the dynamic typed endpoint registry. Sensor, port, and
audio providers register their endpoints there at runtime. `service.audio`
also owns audio-device discovery; devices refer to their capture and playback
stream IDs instead of exposing a board-specific global data path. It has no
board-audio capability requirement. Concrete audio driver packages publish
devices and endpoints when attached, including immutable board-default
attachments for built-in hardware. The independent `service.audio-codecs`
package owns incremental compressed-audio decoding, so file players and
network sources can share the same decoder without owning an audio device.

`expansion.audio-pwm` depends on the generic audio and expansion services. On a
board with expansion PWM it can therefore add a runtime playback device even
when no built-in codec or DAC exists.
`expansion.pcm5102` follows the same ownership model on boards with the
`expansion_i2s` capability and adds an I2S playback device without requiring
built-in audio. `expansion.pcm1808` uses that model for a four-GPIO I2S capture
device. The capability guarantees a spare I2S controller and runtime-safe
GPIOs; individual driver binding validation enforces the required signal
count. Both packages are pruned from boards such as ODROID-GO that cannot
expose an I2S controller for expansion use.
`driver.audio-es8311` provides the `es8311-es7210` and `es8311-duplex` drivers
only for ESP32-S3 targets with I2C and expansion I2S resources.
`driver.audio-esp32-dac` provides `esp32-dac` only for classic ESP32 targets.
Both use the generic audio backend; a board with built-in audio declares a
fixed default attachment instead of compiling a separate board adapter.
The `driver.display-st7305`, `driver.display-st7796`,
`driver.display-ili9341`, `driver.display-cvbs-pal`, `driver.display-vga32`, and
`expansion.ssd1683` packages use the same model. Each package registers an
expansion driver and a board with that integrated panel declares an immutable
early `display0` attachment. SSD1683, ST7305, ST7796, and ILI9341 are available
on both ESP32 and ESP32-S3. The I2S-based CVBS PAL and VGA32 implementations
remain specific to classic ESP32. Generic services do not select these
implementations with driver-specific preprocessor branches.
`expansion.ssd1683` uses a named SPI bus and claimed CS, D/C, reset, BUSY, and
optional power pins. Runtime attachments register an auxiliary display target;
Elecrow declares the same driver as its fixed primary display. Automatic mode
uses changed-frame partial windows.
`expansion.cardkb` polls the M5Stack Unit CardKB at its fixed I2C address and
publishes its character taps and navigation keys through the shared input
service used by shells and foreground apps.
`expansion.gpio-keys`, `expansion.ps2-keyboard`, and `expansion.ps2-mouse`
compose physical buttons, keyboards, and relative mice through the same device
lifecycle. `expansion.analog-joystick` consumes two scalar streams and publishes
normalized axes without synthesizing key events. Board-integrated devices use
the same drivers as default board-selected attachments.
`expansion.sdspi` adds removable SPI microSD storage to boards that do not have
built-in SD hardware. It uses a named expansion SPI bus and mounts at
`/sdcard` without changing the internal-flash root filesystem.
`expansion.sdmmc` provides the native SD/MMC host on ESP32 and ESP32-S3. Boards
with an integrated slot declare a fixed early `storage0` attachment; boards
without one can attach the same driver at runtime. Classic ESP32 accepts only
its native slot-1 pinout, while ESP32-S3 can route the signals through its GPIO
matrix.

## Ownership Rules

- `bootstrap` is immutable and contains only the runtime and shell needed to
  start SolarOS. A flavor cannot disable its members.
- Groups are selection shortcuts only. They cannot own source files or ESP-IDF
  component requirements.
- Every source file and component requirement belongs to a package.
- A driver package can declare its compatible ESP-IDF MCU targets. Target
  pruning occurs before board-capability pruning, so classic ESP32 and ESP32-S3
  implementations are selected before connector and peripheral capabilities
  are considered.
- Expansion-driver symbols belong to their driver packages. The flavor
  generator emits the registry from that metadata; the generic expansion
  service does not include individual drivers behind package `#if` blocks.
- A package lists other packages it needs with `depends`. Enabling an app or job
  automatically enables its transitive dependencies. Explicitly disabling a
  required package is an error.
- Board capability pruning is applied to the resolved graph. If a dependency is
  unavailable, its dependants are removed as well.
- A board can require packages that implement inseparable onboard hardware.
  These packages and their dependencies are enabled in every flavor before
  capability pruning; generation fails if the board cannot support them. For
  example, TTGO VGA32 v1.4 requires the PS/2 keyboard expansion driver used by
  its default `keyboard0` attachment. Built-in defaults are fixed attachment
  instances, not a separate copy of the controller driver.

Groups are the user-facing feature units shared by flavor TOML files and the
configurator. They are deliberately granular: examples include `http_client`,
`ftp`, `logging`, `gameboy`, `pcm1808`, and `st7305`. Categories such as
Networking, Maintenance, Games, and Expansion hardware only organize the
configurator; categories cannot be selected and do not appear in a flavor.
Dormant engineering packages such as USB HID are not exposed as groups.

Groups can contain more than one package when users reasonably perceive one
feature. For example, `ssh` includes the SSH and SCP clients, while `ftp`
includes the client and its server job. Package dependencies can add services
and other internal support automatically. Those packages remain implementation
details rather than becoming additional choices.

The `image_viewer` and `sketch` groups select the two media applications. View
requires graphics and PSRAM for large decoded images. Sketch requires graphics
but has no pointer or PSRAM capability gate: it uses a compact two-bit canvas
and discovers absolute or relative pointers at runtime. Its PNG decoder and
shared storage browser are package dependencies, while mutable app and canvas
state remain cold until the app starts. The four-shade canvas is converted into
a cold-allocated monochrome XBM buffer for one opaque graphics blit per redraw.

The separate `invaders` and `gameboy` groups select those games independently.
Game Boy additionally requires PSRAM, SD storage, and the `streaming_display`
board capability. Its
fixed-frequency emulator submits compact INDEX2 frames to the shared bounded-
cadence presenter. When `service.synth` is present, Game Boy also uses the
MiniGB APU through the shared synth and audio services.

The `writerdeck` flavor targets the Elecrow e-paper board with the `reader`,
`writer`, and `notes` groups plus selected system, maintenance, and network
tools. It excludes general utilities and hardware-diagnostics jobs to stay
focused and fit the board's smaller OTA slot.

The `rover`, `rover-python`, and `rover-lua` flavors target the Freenove
ESP32-WROVER v3.0 composite-video terminal. All three include the expansion
framework and drivers, networking, media viewing, general utilities, Files,
the log job, Bridge, and the GPIO Keys job. The base `rover` flavor also
includes the remaining writing applications. They omit the battery monitor
because the board has no battery hardware, and omit the DAQ and SUMP jobs by
default. The Logic app is also omitted because its timing-sensitive capture
buffer requires more internal-memory margin than this configuration provides.
`rover` has no embedded interpreter; `rover-python` and `rover-lua` omit
Reader, Writer, and Notes, and add only their selected scripting stack. These
three general-purpose images omit games. `rover-gameboy` is the focused PAL
game image: it enables `gameboy` but leaves `invaders` out to fit the 4 MB
board while retaining basic files, Wi-Fi, and SSH/SCP tools. Agent remains
excluded because its runtime memory requirements exceed the practical
internal-memory margin. The board's 4 MB flash uses one
large factory application slot without OTA; these flavors also omit remote
manual synchronization. The embedded `docs` application remains available.

`rover-synth` is a focused classic-ESP32 synthesizer build. It retains BLE and
PS/2 keyboard input, SD and basic file tools, controls, MIDI, and the LEDC PWM
audio expansion while omitting Wi-Fi/networking, packet radio, media, games,
scripting, and unrelated application groups. Composite video owns I2S0, so the
on-board audio package remains disabled and Synth uses a runtime-attached PWM
output. This smaller profile preserves Bluetooth controller memory headroom.

Network ownership is intentionally split. `network.base`, `service.osc`, `job.osc`, `network.wireguard`, `network.mqtt`,
`network.ssh`, `network.mail`, `messaging.gateway`, `network.http-client`, and
`network.http-server` own their individual implementations. Image and document
decoding are separate `media.image` and `media.document` packages, so selecting
`app.curl`, for example, does not pull MQTT, SSH, mail, or image dependencies.

`service.osc` owns the bounded OSC 1.0 codec, incoming native-parameter
mapping, named volatile outbound bindings, and the `osc` shell command.
`job.osc` owns the IPv4 UDP socket, peer filtering, rate limit, source sampling,
and counters. It consumes the existing stream and control services without
making either core service depend on networking.

`network.wireguard` depends directly on `service.wifi` and lwIP. It owns the
native tunnel, its bounded route table, the lwIP route hook, persistent client
profile, Wi-Fi address-event handling, and light-sleep lifecycle. It is not part
of either embedded scripting runtime.

`network.http-client` owns the shared TLS-enabled HTTP transport used by `curl`,
`webradio`, and `web`. It exposes request headers and bodies, redirects,
streaming response events, cross-task cancellation, per-I/O timeouts, and an
end-to-end deadline.
Callers continue to own their worker task and response consumer; see
[HTTP Client Service](../http_client.md) for the native API and lifecycle.

`service.webradio` owns the disk-backed user station catalog on the current
storage root. `app.webradio`
combines that catalog with the shared HTTP client, MP3 codec, generic audio
output service, and `service.signal-widgets`. The signal-widget package owns
reusable signed-16-bit oscilloscope and DSP spectrum components; it depends on
`service.dsp`, whose eligible ESP32-S3 FFT path uses PIE SIMD. WebRadio requires
Wi-Fi but does not require the board-audio capability: a headless board can
include the app and later gain a default audio output from a runtime-attached
expansion. The foreground app owns separate network/decode and steady playback
workers joined by a PSRAM-preferred PCM jitter buffer. Suspending its UI leaves
those workers running; closing the app stops them and releases their resources.

`app.player` combines the shared audio and codec services with a persistent
playlist on the current storage root, the reusable storage browser, signal
widgets, and the reusable cassette widget. Its file browser selects existing
WAV/MP3 files. Player has no built-in-audio capability requirement, so a
runtime-attached output device can satisfy playback.

`app.recorder` is the interactive counterpart to `arecord`. It combines the
generic audio and stream services with the same storage browser, cassette,
oscilloscope, and spectrum packages as Player. It records PCM WAV from a
runtime-selected capture stream and does not require built-in board audio.
Its monitor transport loops the selected capture stream to the current default
output and visualization widgets without opening a recording file. Recorder
allocates its app-lifetime state in PSRAM on PSRAM boards and releases it when
the application closes. Its browser, widgets, conversion buffers, and PCM queue
also use PSRAM. Its 8 KiB capture/file worker and the reusable audio-player
sink are admitted as user-started foreground transports with internal stacks,
and continue while the Recorder UI is suspended. Capture and sink timing, plus
cache-disabled filesystem access, therefore do not depend on a PSRAM stack.
Recorder stores its selected input, destination folder, format, device gain,
volume, and visualizer under `.recorder` on the current storage root. Its
cassette animates only for record and playback; scope and spectrum continue to
present monitored input at the app's nominal 25 Hz graphical cadence.
Hardware input gain is exposed through an optional audio-device operation;
streams without a gain-capable owning device remain valid recording inputs and
show gain as unavailable.

`app.funcgen` combines `service.synth` with the shared oscilloscope widget.
It has no built-in-audio capability requirement and discovers runtime output
devices when opened. Its foreground and widget state prefer PSRAM; the bounded
oscillator state and Synth worker remain internal for deterministic rendering.
Every displayed control has a corresponding native parameter for physical and
MIDI control bindings.

The `agent` group selects `app.agent`; its `service.agent` dependency is added
automatically.
`service.agent` owns provider-neutral events, NVS-backed provider
configuration, bounded tool-loop policy, a declarative typed-tool registry,
and the OpenAI Responses/Chat-Completions adapter. The registry owns provider
schemas, output schemas, risk and availability metadata, and shared execution
for the system, storage, jobs, and policy-gated script tools. Its NVS-backed
`off`, `readonly`, `confirm`, and `all` policy filters advertised schemas and
is enforced again at execution time.
It depends on the shared HTTP and JSON services and is pruned from targets
without both Wi-Fi and PSRAM. Python and Lua are not dependencies. See
[Native Agent Service](agent.md).

`app.python` and `app.lua` each depend on `service.script_runner`. That service
defines the common source/file request, bounded output, cancellation, deadline,
and completion-status contract. Each interpreter owns its language adapter and
single-owner guard. `app.agent` supplies installed adapters to both the manual
`agent script` command and the typed agent registry without making either
interpreter a dependency of the agent package.

Messaging is split into explicit layers: `service.messaging` owns
provider-neutral conversations, history, delivery state, and the pending
outbox; `messaging.gateway` owns gateway configuration and room commands;
`chat.transport.gateway` owns the gateway wire protocol; and
`job.gateway-sync` owns connection lifetime, retries, cursors, and gateway
delivery. The bounded store persists full messages on SD and uses the
Inbox's compact persistent records as its internal-flash fallback. Stable
producer IDs suppress transport replays before they reach either UI. `app.chat`
depends only on the provider-neutral Messaging service and is a foreground view
over that shared state; `job.chatd` remains the
independent local gateway server.

Inbox storage and presentation are also separate. Producers such as mail and
POCSAG depend only on `service.inbox`; it owns the bounded persistent ring under
`/.inbox/`, durable read state, producer-key replay suppression, and the
persisted opt-in notification-sound policy. Audio-capable builds enqueue the
sound through `service.audio`; Inbox remains available without audio hardware.
`app.inbox` adds the foreground browser and its shell command.

`service.synth` depends on the device-independent `service.audio` and provides
exclusive real-time PCM rendering for reusable synthesizers and emulated sound
hardware. It opens the default playback endpoint at runtime, so firmware
without built-in board audio can use a subsequently attached output expansion.
The selected audio provider retains hardware-codec, global-volume, and
output-serialization ownership; the synth service owns a bounded render block,
dedicated worker, client ownership, and deadline/error counters. Native apps
can supply an independent signed 16-bit stereo render callback.

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
  --input flavors/core.toml \
  --output flavors/my-flavor.toml \
  --board waveshare_esp32_s3_rlcd_4_2 \
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

For maintainer experiments, an optional `[packages]` table can override
individual internal packages. The builder does not expose that table as
another selection layer.

Use `pkg` on the device to inspect the resolved package list.

## Quick reference

Select a board, an update layout, and then granular groups. Build to replace the
total estimate with a measured image size; adjust, rebuild, and flash when it
fits. A flavor stores only portable groups. The board supplies required hardware
drivers and removes unsupported groups, while the layout defines the image
limit. Internal packages and services are resolved automatically. Use `pkg`
on-device to inspect the resolved firmware.
