# Package architecture

Maintainer reference for package ownership and service boundaries. For choosing
firmware features and building an image, see the
[packages and flavors user guide](manual/packages.md).

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
  [Native libraries](manual/native-library.md). A per-entry lifecycle ABI is distinct
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
are installed as [external foreground apps](manual/native-app.md) through the module
catalog and require native-module support in the firmware. They do not add
firmware flavor groups.

## Service boundaries

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
[Native Agent Service](manual/agent.md).

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

USB host input and storage are provided by `service.usb-host` and
`service.usb-storage`. The latter depends on the host service. USB HID
peripheral output is the separate dormant `service.hid` package.
