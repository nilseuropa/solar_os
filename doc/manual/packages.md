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

A **flavor** selects which features are compiled into a firmware image.
A **group** is a convenient selection, such as `scripting` or `vision`.
A **package** supplies a particular app, job, service, or driver.
Required dependencies are included automatically; unsupported packages are
removed for the selected board.

## Inspect the running image

```text
pkg system
board
apps
jobs
```

`pkg system` shows compiled packages. `pkg installed` lists separately installed
native modules. A compiled driver is available to use, but does not mean its
hardware is attached; check `expansion devices`.

## Choose features

Use the [SolarOS Builder](#solaros-builder) to select a board and flavor, or
create a custom flavor below. The `full` flavor includes these optional groups
where the board supports them:

| Group | Use | Guide |
| --- | --- | --- |
| `vision` | Decode QR codes and process images. | [Computer vision](vision.md) |
| `inference` | Run resident models on ESP32-S3 with PSRAM. | [Model inference](inference.md) |
| `inference_backend` | Include the ESP-DL engine in firmware; otherwise install the `espdl` library. | [Model inference](inference.md) |
| `jet` | Include the script/display frontend for the separately installed Jet library. Also selected by Python and Lua on supported boards. | [Jet 3D renderer](jet.md) |
| `zoo` | Browse and install model bundles. | [Zoo](apps.md#zoo) |
| `pipelines` | Process images or video in the background. Also select the desired processor. | [Image pipelines](pipelines.md) |
| `servo` | Control calibrated positional servos. | [Servo outputs](servo.md) |
| `pwm` | Use native PWM outputs where supported. | [GPIO and PWM](gpio.analog.md) |
| `pca9685` | Add 16 PWM outputs over expansion I2C. | [PCA9685](servo.md#pca9685-expansion-controller) |
| `drv8830` | Control brushed DC motors over named I2C buses. | [Motor control](motor.md) |
| `motor` | Common motor service, shell controls and Python/Lua APIs. | [Motor control](motor.md) |
| `encoder` | Common encoder counts, shell controls and Python/Lua APIs. | [Encoder counts](encoder.md) |
| `pcnt_encoder` | Attach native A/B encoders with explicit x1/x2/x4 counting. | [Encoder counts](encoder.md) |
| `ds3231` | Add a battery-backed I2C clock and hardware alarm. | [DS3231](expansion.md#ds3231-real-time-clock) |

Python or Lua alone does not enable optional media, vision, or inference
services. Stored-image processing needs no camera or display. Audio can use
an expansion output on a board without a built-in speaker.

For external hardware, select its driver group and check the board's expansion
capabilities and available pins. Built-in hardware uses fixed instances of the
same drivers. See [Expansion](expansion.md) for attachment commands and
[Boards](boards.md) for hardware limits.

## Build reference

Package definitions live in `packages/solar_os_packages.toml`; flavor files
normally select groups. The following rules apply when maintaining these build
files.

## Ownership Rules

- `bootstrap` is immutable and contains only the runtime and shell needed to
  start SolarOS. A flavor cannot disable its members. On ESP32-S3 boards with
  PSRAM it also contains the native-module loader service and its `load`
  maintainer command, plus signed-catalog installation through `pkg`; target
  and capability pruning remove those services from other boards.
- Native module manifests and signed catalogs carry an explicit `app`, `job`,
  `driver`, or `library` type. Each type has a versioned runtime contract. Installed
  artifacts use separate `/modules/apps`, `/modules/jobs`, and
  `/modules/drivers`, and `/modules/libraries` namespaces. Application modules run synchronously and
  unload; jobs and drivers remain resident while their callbacks are
  registered. Libraries load on first acquisition and remain resident until
  unloaded; leases protect their API while consumers use it. See
  [Native libraries](native-library.md). A per-entry lifecycle ABI is distinct
  from the native host ABI.
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
`ftp`, `logging`, `invaders`, `pcm1808`, and `st7305`. Categories such as
Networking, Maintenance, Games, and Expansion hardware only organize the
configurator; categories cannot be selected and do not appear in a flavor.
Dormant engineering packages such as USB HID are not exposed as groups.

Groups can contain more than one package when users reasonably perceive one
feature. For example, `ssh` includes the SSH and SCP clients, while `ftp`
includes the client and its server job. Package dependencies can add services
and other internal support automatically. Those packages remain implementation
details rather than becoming additional choices.

The `launcher` group selects the native graphical launcher and its JSON parser.
Board capability pruning removes it from builds without graphics. Its mutable
configuration and item state remain cold until the app starts.

The `image_viewer` and `sketch` groups select the two media applications. View
requires graphics and PSRAM for large decoded images. Sketch requires graphics
but has no pointer or PSRAM capability gate: it uses a compact two-bit canvas
and discovers absolute or relative pointers at runtime. Its PNG decoder and
shared storage browser are package dependencies, while mutable app and canvas
state remain cold until the app starts. The four-shade canvas is converted into
a cold-allocated monochrome XBM buffer for one opaque graphics blit per redraw.

The `invaders` group selects the built-in Invaders game. Game Boy and Apple II
are installed as [external foreground apps](native-app.md) through the module
catalog and require native-module support in the firmware. They do not add
firmware flavor groups.

SolarOS provides three firmware profiles. `full` is the standard profile with
the broad application and hardware-service selection. `classic` uses the compact
Writerdeck feature selection without the synthesizer and with ADC D-pad support
for ODROID-GO. It is the default for all classic ESP32 targets, including boards
with 4 MiB flash.
`writerdeck` is the compact profile used by e-paper boards with smaller
application partitions.

`classic` retains Python, Playground, writing, media, communications, and the
selected board's required hardware drivers. Lua, built-in games, video playback,
and image-processing pipelines are omitted. ODROID-GO retains its buttons and
ADC D-pad; VGA32 retains its default PS/2 keyboard attachment.

`writerdeck` includes Python, Playground, editing, Files, Reader, Writer, Notes,
View, audio playback, recording, speech synthesis, the synthesizer, Web, Agent,
WireGuard, messaging, and file transfer.
Its synth service also supplies generated audio for native foreground apps.
Its transfer tools include FTP/FTPD, SSH, SCP, SFTP, SFTP Sync, and Curl.
PCM5102/I2S and PWM audio remain available for runtime expansion outputs.
Broad expansion-driver collections, external radio stacks, Lua, built-in
games, and RTSP are omitted. Board and target requirements still
prune unsupported features from every profile.

On ESP32-S3 boards with PSRAM, all profiles include `pkg` for installing
native modules. Native-module installation is unavailable on classic ESP32;
Python and Playground remain available on its PSRAM boards. On supported
targets, all profiles use the downloadable `espdl` library for inference.
Custom profiles can select `inference_backend` to include the ESP-DL engine
in firmware.

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

`network.wireguard` depends on the generic network service and lwIP. It owns the
native tunnel, its bounded route table, persistent client profile, preferred
underlay changes, and light-sleep lifecycle. The network service owns the shared
lwIP route hook used by Wi-Fi, PPP, WireGuard, and other IP paths. WireGuard is
not part of either embedded scripting runtime.

`network.http-client` owns the shared TLS-enabled HTTP transport used by `curl`,
`webradio`, and `web`. It exposes request headers and bodies, redirects,
streaming response events, cross-task cancellation, per-I/O timeouts, and an
end-to-end deadline.
Callers continue to own their worker task and response consumer; see
[HTTP Client Service](https://github.com/nilseuropa/solar_os/blob/main/doc/http_client.md) for the native API and lifecycle.

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

For maintainer experiments, an optional `[packages]` table can override
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
