+++
id = "jobs.reference"
title = "Background job reference"
section = "job"
summary = "Configuration, ownership, and examples for every background job"
aliases = ["job.reference"]
keywords = "jobs background workers start stop status memory ports examples"
packages_any = []
+++
# SolarOS Jobs

This document covers the built-in background job registry. Jobs are autonomous
workers such as log followers, DAQ capture, HTTP serving, Telnet shell access,
SLIP, PPP, chatd, or NTP sync. Foreground applications are documented in
[apps.md](apps.md), and shell commands are documented in
[commands.md](commands.md). Port shells are sessions, started with
`session create shell <port>` plus optional `--term`, `--charset`, and `--size`
terminal settings. Display-target shells use `session create shell <target>`.

Job availability depends on the selected firmware flavor and board
capabilities. The running system is authoritative:

```text
jobs
job status [name]
```

`jobs` shows state, worker-stack bytes, and resource counts. Use
`job status NAME` for errors, resource owners, memory placement, and timings.

## Job Control

| Command | Description |
| --- | --- |
| `jobs` | List registered jobs in compact form. |
| `job status [name]` | Show all jobs, or one named job with details. |
| `job start <name> [args...]` | Start a job with optional arguments. |
| `job stop <name>` | Stop a running job. |

Only one instance of each job name can run at a time. Starting a running job
first stops the previous instance, then starts it again with the new arguments.

Jobs report which ports, files, streams, and listeners they own. If a resource
is busy, stop its current user before starting another job on it.

- `waiting`: the start request is retained and retries automatically when
  enough worker memory is available.
- `failed`: starting or running failed. Check `job status NAME`, fix the cause,
  and restart. Jobs may need buffers beyond their declared stack size.
- `running`: the job started successfully.
- `stopped`: there is no active job or pending start request.

The `STACK` column is the declared worker-stack admission requirement in bytes,
not a worst-case total-memory estimate. `-` means that the job declares no
dedicated worker stack; it may still allocate dynamic buffers or use a shared
service. Compare this column with the internal figures from `mem policy`.
For an internal-stack background job, admission currently requires internal
free memory of at least `STACK + 1 KiB overhead + 32 KiB reserve`, and an
internal largest block of at least `STACK + 1 KiB overhead`. Treat a manual
check as a snapshot: other work can allocate memory before `job start`, which
is why SolarOS retains and retries a launch that loses that race.

Timing fields show the effective interval/deadline, last and maximum execution
time, and missed deadlines. A missed deadline is recorded; it does not forcibly
stop a handler.

Compact list example:

```text
NAME         STATE    STACK KIND        EVT  TICKS RES
batmon       running      - background  tick    17   1
log          stopped   6144 background  tick     0   0
```

Detailed status example:

```text
job status log
NAME         STATE    STACK KIND        EVT  TICKS RES
log          running   6144 background  tick     8   1
  summary: stream SolarOS logs to a port or file
  owner: job:log
  worker stack: 6144 bytes (internal)
  tick: 250/2ms n=8 us=18/31 miss=0
  resources:
  - port   cdc0 rw
```

Useful ports:

```text
cdc0
uart0
```

List available streams with:

```text
stream
```

## Startup

Jobs stop at reboot. Run `setterm startup` to find the selected startup script
and add the commands you want to run at boot. For example:

```text
wifi on
job start ntp-sync once
job start batmon 60
```

## batmon

Battery monitor. It periodically samples battery voltage, maintains a smoothed
trend, estimates power state, and can request sleep, deep sleep, or shutdown
when the configured minimum voltage is reached.

Usage:

```text
job start batmon [interval-sec] [sleep|deepsleep|shutdown]
job stop batmon
job status batmon
```

Defaults:

| Setting | Value |
| --- | --- |
| Interval | `60` seconds |
| Low-voltage action | `sleep` |

The action can also be supplied without an interval, for example
`job start batmon shutdown`. The selected action appears in the startup log
and the job's battery resource details.

| Action | Behavior |
| --- | --- |
| `sleep` | Request the same light sleep as the `sleep` command. |
| `deepsleep` | Request the same deep sleep as the `deepsleep` command. |
| `shutdown` | Run the normal shutdown sequence, including storage cleanup and board power-off when supported. |

Battery limits are configured with the `battery` shell command:

```text
battery capacity <mAh>
battery min_voltage <volts>
battery max_voltage <volts>
```

Notes:

- Discharging trend means battery power.
- Charging trend means external power.
- Three consecutive samples at or below `min_voltage`, with an established
  discharging trend and no inferred external power, request the selected action.
- The voltage trend needs at least five minutes of history. This check runs
  only while `batmon` is running.
- Set `min_voltage` above the board's minimum operating voltage, with margin
  for voltage drop under load. Changing the action does not change the threshold
  or bypass the trend and consecutive-sample checks.

Example:

```text
job start batmon 60
job start batmon 30 shutdown
```

## bridge

Bidirectional byte bridge between two byte-stream ports, or between one
byte-stream port and an active SolarOS Link instance.

Usage:

```text
job start bridge <port-a> <port-b>
job start bridge <port> <link> [broadcast|destination-id]
job stop bridge
job status bridge
```

Example:

```text
job start bridge cdc0 uart0
```

To expose a UART byte stream over a packet-radio Link:

```text
job start radio-link link0 radio0 lora-eu868
job start bridge uart0 link0 broadcast
```

That direct Link form is best-effort. For an ordered, retransmitted stream,
create a peer-bound virtual port and use the normal port-to-port bridge form:

```text
link stream create link0 vser0 0x12345678
job start bridge cdc0 vser0
```

The remote device creates its matching `vser0` and can attach a normal shell
with `session create shell vser0 --term dumb`. On a headless DevKit, this leaves
the primary `uart0` shell free for administration while Linux uses USB `cdc0`
for the remote terminal.

Use a decimal or `0x` 32-bit Link destination instead of `broadcast` for
acknowledged unicast:

```text
job start bridge uart0 link0 0x12345678
```

Notes:

- The two ports must be different.
- Both ports are claimed by the bridge job until it stops.
- Link stream ports such as `vser0` are normal byte-stream ports. Their stream
  service supplies peer filtering, segmentation, ordering, retransmission, and
  bounded backpressure before the bridge sees bytes.
- In Link mode, the serial port is claimed while the already-running Link
  instance remains active under its transport job.
- Available serial bytes are emitted as binary Link messages, each capped at
  the Link payload MTU. Received text and binary payloads are written to the
  serial port without a separator, preserving byte-stream behavior.
- `broadcast` is the default destination. Explicit destinations request normal
  Link acknowledgements.
- The bridge consumes the Link receive queue. Do not use `link receive` on the
  same Link while the bridge is running.
- Packet radio is usually much slower than UART. The bridge uses the existing
  bounded Link queues and does not add an unbounded SRAM buffer; sustained
  serial input can therefore overrun the port or produce Link queue drops.
- If the Link disappears, the bridge releases its serial port and stops with
  the Link error.
- This is the clean base for USB-to-UART converter style workflows.

## irc-sync

Maintains enabled native IRC instances over verified TLS. Each instance has
independent reconnect and channel membership state. Configure profiles with
`irc`, then run `job start irc-sync`. The job accepts no arguments.
Use `irc status` for each connection and `job stop irc-sync` to stop all IRC
connections. Chat and retained messages remain available. See [Native IRC](messaging.irc.md).

## gateway-sync

Background synchronizer for the gateway messaging provider. Start and stop it
explicitly, using the same lifecycle as `email-sync`:

```text
job start gateway-sync
job stop gateway-sync
job status gateway-sync
```

`gateway-sync` takes no polling interval. Unlike the periodic `email-sync` job, it
maintains a live connection and applies its own exponential reconnect backoff.
It can therefore be started before Wi-Fi has an address; it remains running and
connects when the network becomes available. In `/.shell/startup`, use exactly:

```text
job start gateway-sync
```

It owns transport connection lifetime, exponential retry, opaque resume
cursors, joined-channel replay, and delivery of the gateway provider's shared
outbound requests. The messaging service owns retained publication and Inbox
projection. Replayed transport messages are
deduplicated by the shared stable producer identity before another notification
is published.

The gateway hello uses the SolarOS `identity user` and `identity hostname`
values. Chat does not maintain separate user-name or device-name settings.

Stopping or closing `app.chat` has no effect on this job. Its worker performs
transport startup, polling, and retry work outside the cooperative session/job
scheduler.

The shared store retains at most 64 messages. SD-backed systems use the
full-message `/.messages/messages.bin` ring. Systems using internal flash
restore Chat history
from the compact records already stored in `/.inbox/messages.bin`; no second
ring is created, so Chat history cannot consume the remaining flash volume.

## chatd

Local SolarOS chat gateway server. It is useful for testing the `chat` app or
for small trusted local networks.

Usage:

```text
job start chatd [port] [token] [--history path] [--sc2 on|off] [--v1 on|off] [--sc2-id ID]
job start chatd [port] [token] [path]
job stop chatd
job status chatd
```

Defaults:

| Setting | Value |
| --- | --- |
| Port | `7777` |
| Default channel | `general` |
| Maximum clients | `6` |
| Maximum channels | `32` |
| In-memory history | `64` events |

Arguments are intentionally flexible. The first numeric argument is the port.
The next non-option argument is the optional token. `--history` or `--log`
selects an optional append-only history dump file.

Examples:

```text
job start chatd
job start chatd 7777 secret
job start chatd 7777 secret --history /.shell/chatd.log
```

### SC2 mini-relay

On builds with SC2 messaging, chatd is also a small SC2 relay on the same
port, so a device can be a relay and a client at once. The first line a
client sends decides: an SC2 `hello` goes to the mini-relay, a v1 `hello`
to the v1 server. Legacy v1 stays on by default, as on the self-hosted
relay; `--v1 off` refuses v1 clients (426) and `--sc2 off` turns the
mini-relay off. The token, when set, is also the SC2 admission code for
new devices. Rate limits are off, as on the self-hosted relay.

The mini-relay is a fixed-size subset of the relay protocol:

| Feature | Limit |
| --- | --- |
| Login and auth (signed `welcome`, pinned relay key) | 6 connections |
| Registered devices | 16 (then `503 server_full`) |
| Prekeys per device (bundle publish and claim) | 8 one-time prekeys |
| Offline DM mailbox (store and forward) | 24 messages, 12 per device, 3200-byte envelopes |
| Public room `pub:general` | last 24 posts |

The six connections are held for a bounded time, as on the self-hosted
relay:

- One session per device: when a device logs in again, its older session
  gets `409 replaced` and is closed (this also clears a connection left
  half-open by a network change).
- Keepalive pings are not activity. A session that sends nothing else for
  5 minutes gets `408 idle` and is closed; the client reconnects and picks
  up waiting DMs at its next sync. A login has to finish within 30 seconds.
- When all six connections are taken, a new one takes the slot of the
  session that has been idle longest, if that one has been idle for at
  least a minute (`503 evicted`); otherwise the newcomer gets
  `503 server_full`.

Private rooms and other public rooms are refused (`501`/`404`); use a full
relay for those. All stores live in one PSRAM block (about 135 KiB), so the
mailbox and history are lost when chatd stops. Without PSRAM the mini-relay
stays off and v1 keeps working. The relay key is created once and kept in
the SC2 store (`mr/relay`), so pinned clients keep trusting it across
restarts. chatd logs it at start:

```text
chatd: SC2 relay chatd-1a2b3c4d key=B64 (134 KiB PSRAM), v1 too
```

Pin it on clients with `messages add sc2 NAME chat://IP:7777 [TOKEN] key=B64`.
The device running chatd can use its own relay through
`chat://127.0.0.1:7777`.

The local chat app can connect with:

```text
chat local
chat 127.0.0.1:7777
```

On another SolarOS device or host on the same network, use the server IP:

```text
chat 192.168.1.113:7777
```

Notes:

- If a token is configured, clients must present the same token.
- New clients receive the recent in-memory channel history.
- Channel deletion is supported by the chat protocol and client.
- The built-in server is a lightweight LAN gateway, not a hardened public chat
  service.

## daq

Data acquisition job. It captures scalar and event streams to timestamped CSV,
or one byte or PCM audio source directly to a raw file.

The `daq` shell command is usually easier to remember:

```text
daq
daq streams
daq start <stream...> <file> [options]
daq start <file> <stream...> [options]
daq stop
daq status
```

Direct job usage:

```text
job start daq <stream...> <file> [--rate seconds|--rate-ms ms] [--append|--replace]
job start daq <file> <stream...> [--rate seconds|--rate-ms ms] [--append|--replace]
job start daq <byte-stream> <file> --raw [--rate-ms ms] [--append|--replace]
job start daq <audio-stream> <file> --raw [--rate-ms ms] [--append|--replace]
job stop daq
job status daq
```

Defaults:

| Mode | Default interval |
| --- | --- |
| Scalar CSV | `1000` ms |
| Raw byte stream | `25` ms |
| Raw audio stream | Continuous |

Examples:

```text
daq start temperature /logs/temp.csv --rate 60
daq start /logs/env.csv temperature humidity battery --rate 60
daq start uart0 /logs/uart0.bin --raw --rate-ms 25
daq start audio0.capture /logs/microphones.pcm --raw
job start daq /logs/env.csv temperature humidity battery --rate 60
```

Notes:

- Multi-stream mode supports scalar and event streams only.
- Raw capture is single-stream only and writes byte or PCM audio data directly.
- Raw audio files contain the native format shown by
  `stream status audio0.capture`; use `arecord` when a WAV container is needed.
- CSV rows include a timestamp column and one value column per stream.
- Available streams depend on board capabilities.

## controls

Continuous-control mapper. It samples every configured scalar-stream control
at 50 Hz, applies smoothing, deadband, calibration, and inversion, then updates
changed native parameter and MIDI CC bindings.

```text
control create cutoff adc1 0 3300 smooth=40 deadband=8
control bind cutoff parameter synth.filter.cutoff pickup=on
job start controls
job status controls
job stop controls
```

The job takes no arguments. Controls and bindings can be added or removed while
it runs. A source read error and an unavailable target are retained in control
and binding status instead of stopping the worker. Native parameter bindings
retry when the application resumes and publishes the path again. MIDI bindings
retry while the MIDI worker is stopped.

Control definitions are runtime configuration. Put the `control create`,
`control bind`, and `job start controls` commands in `/.shell/startup` to
restore a hardware setup after reboot. See `man controls` for calibration,
manual script inputs, MIDI examples, and inspection commands.

## gestures

Gesture-to-command listener. Rules are configured independently with `gesture
bind`, while this job owns observation and dispatch:

```text
gesture bind source=gesture0 gesture=flick direction=east -- input emit ALT+RIGHT
gesture bind source=gesture0 gesture=flick direction=west -- input emit ALT+LEFT
job start gestures
job status gestures
job stop gestures
```

Stopping the job preserves the rules, prevents new gesture actions, and drops
queued actions. A command already executing finishes normally. Use `gesture
bindings` to inspect rules and counters, `gesture unbind <id>` to remove one, or
`gesture unbind all` to clear the table and reset the next ID to 1. Rules and job
state are volatile; recreate them in the startup script when persistence is
needed. The command worker is created on demand and releases its internal stack
when its queue is idle.

## osc

OSC 1.0 IPv4 UDP adapter for automatic incoming native-parameter writes and
explicit named outbound stream, event-stream, or normalized-control bindings.

```text
job start osc [listen=port] [target=host:port] [peer=ipv4]
job status osc
job stop osc
```

The listening port defaults to `9000`. `target=` is optional for an
incoming-only job and is required before an outbound binding can send.
`peer=` accepts one exact IPv4 address and drops all other incoming sources.

The worker owns one UDP socket and a 6 KiB internal stack. It accepts packets
up to 512 bytes, at most eight parameter updates per packet, immediate bundles
only, and at most 100 accepted packets per second. Detailed status includes the
listener, target, peer filter, inbound apply/error counters, outbound
send/source errors, and the current binding count.

OSC has no authentication or encryption. Start the job only on a trusted LAN,
SoftAP, or WireGuard path. Bindings are volatile and can be restored from
`/.shell/startup`. See `man osc` for address mapping, binding syntax, limits,
and the sampled-event caveat.

## cam-webd

HTTP access to the fitted camera as one-shot JPEG images or a single-client
MJPEG stream.

```text
job start cam-webd [qvga|vga] [fps] [auth=none|required]
job status cam-webd
job stop cam-webd
```

The defaults are QVGA JPEG at five frames per second with no authentication.
The optional frame rate is `1..30`; JPEG quality is fixed at 12. Options can be
given in any order. The job leases the camera as `job:cam-webd` until it stops.
The shell `camera` command and other camera users report that owner while the
lease is active.

API:

```text
GET /api/camera
GET /camera.jpg
GET /camera.mjpeg
```

With the default `auth=none`, the endpoints can be opened directly:

```text
http://device/camera.mjpeg
```

Use `auth=required` to require `Authorization: Bearer <code>` on all three
endpoints. Starting in that mode prints a random six-digit access code. For
example:

```text
curl -H 'Authorization: Bearer 123456' http://device/camera.jpg -o frame.jpg
curl -H 'Authorization: Bearer 123456' http://device/camera.mjpeg -o stream.mjpeg
```

The MJPEG endpoint uses the shared HTTP server's asynchronous request path.
Only one stream client is admitted. The worker captures and transmits one
camera framebuffer at a time and releases it on every success or error path;
there is no frame queue or JPEG copy. A slow client therefore reduces the
capture rate through socket backpressure instead of consuming more memory.
Snapshot requests are rejected while the stream owns the capture path.

The server is plain HTTP. With the default `auth=none`, anyone who can reach the
device can view the camera. Use it only on a trusted Wi-Fi network. The optional
access code limits casual access but does not encrypt images and is not intended
for exposure to an untrusted network.

## rtspd

Publish explicitly selected sources as a standard single-client RTSP session.
Video uses RTP/JPEG; audio uses RTP/L16 PCM. Each enabled track has its own
RTP/RTCP UDP port pair, SSRC, and sender reports, with a shared RTCP CNAME and
session start clock.

```text
job start rtspd [video=<stream>|none] [audio=<stream>|none] [size=qvga|vga] [fps=0..30] [port=<port>]
job start rtspd video=camera0 audio=none size=qvga fps=10
job start rtspd video=none audio=audio0.capture
job start rtspd video=camera0 audio=audio0.capture
job status rtspd
job stop rtspd
```

Defaults are `video=camera0 audio=none size=qvga fps=5 port=554`. `video=camera`
is a compatibility alias for `camera0`. Video selects a typed JPEG frame source
from `stream list`; opening it holds the same exclusive hardware lease as the camera
service. At least one source must be enabled. `fps=` is a maximum video
publication rate, not a
capture timer; `fps=0` removes the cap. `size=` and `fps=` are invalid with
`video=none`. JPEG quality is fixed at 12.

Use `stream list` to find available video and audio source IDs. `audio0.capture`
is the board codec's PCM microphone endpoint when available; `mic0` is a scalar
level sensor, not a PCM source. Audio must be a source or
duplex S16LE PCM endpoint with 16-bit samples, 1..8 channels, and a native rate
of 8000..192000 Hz. The job advertises the source's native rate and channels;
it does not resample. Only selected hardware is leased as `job:rtspd`.
Tab completion after `job start rtspd` offers unused option keys. After `audio=`
or `video=`, double Tab lists compatible registered source IDs and `none`;
partial IDs complete normally. Selection does not open or lease the source.
Audio-only publishing requires neither a camera nor the camera package.
For playback on another SolarOS device, use JPEG video and L16 audio at
8000..48000 Hz with one or two channels. A 16000 Hz mono source is the
recommended low-bandwidth audio format; native 16000 Hz stereo capture also
works. The publisher's wider native-format support does not imply that every
receiver supports those formats, and selecting a source never resamples it.
External publishers should use baseline JPEG with both quantization tables;
160x120 at 10 fps is a useful initial ESP32 receiver workload. RTP packets must
fit the path MTU; SolarOS publishes packets of at most 1200 bytes.
Selecting an absent, busy, sink-only, or incompatible source fails startup;
there is no silent fallback to another source.

Reader workers block on camera/audio availability and immediately publish or
discard the completed frame/block. They continuously drain idle sources, so
connecting does not expose a stale camera image or audio backlog. The video
cap drops newly captured images rather than holding them until a timer expires.
There is one leased PSRAM camera framebuffer, no JPEG copy or video queue,
and at most one MTU-sized PCM block. Disabled sources allocate no reader stack.
Temporary UDP transmit pressure drops the current frame/block and newly
captured data during a short backoff instead of disconnecting or building a
queue. Audio sample timestamps advance across dropped blocks. Status reports
congestion drops and the last transmit errno separately from fatal send errors.
Each enabled source uses an additional 4096-byte internal worker stack, shown
by `job status rtspd`, and is closed by its owning reader during cancellation.
Source stack minimum-free values are reported in bytes, including microphone
startup and publishing. Packet scratch and RTSP session storage are allocated
only when the job starts, using the PSRAM-preferred memory policy; disabled
sources allocate no scratch. Audio scratch is one 1188-byte PCM block plus
one 1200-byte RTP packet, off the reader's stack. I2S DMA buffers and task
stacks remain internal. Runtime buffers are freed after workers and leases
have closed, including failed startup; a pending stop retains them safely.
An unused/stopped job reserves no session or media buffers, only its small
internal control state and lock. `job status rtspd` reports runtime buffer sizes.

Open the single RTSP client session:

```text
vlc rtsp://device/media
rtsp rtsp://device/media
ffplay -rtsp_transport udp -fflags nobuffer -probesize 32 -analyzeduration 1 -max_delay 100000 rtsp://device/media
```

FFplay's default probing/playback buffers can add seconds of latency; the
example reduces client-side probing and UDP reordering delay. It does not
guarantee a particular glass-to-glass latency. Capture timestamps determine
video RTP timing; audio timestamps advance by sample frames. Images and audio
blocks predating each `PLAY` are discarded.

The camera lease excludes `cam-webd`, shell camera capture, and other camera
owners only when a camera source is selected. Each packet is at most 1200 bytes.
Unsupported JPEG modes are counted and dropped, not sent using private payloads.
Transport is UDP; RTSP-over-TCP interleaving is unsupported. VLC requires a
build with Live555: if its log reports `satip` or `access_realrtsp` failures,
check for `--disable-live555` and use FFplay or a compatible VLC build.

The stream is unauthenticated and unencrypted; use it only on a trusted LAN
or protected network path.

## displayd

Authenticated HTTP display and remote control. It has two modes:

- With a physical target such as `display0`, it mirrors and controls the
  active session attached to that display without allocating another display
  framebuffer.
- With `web0`, it creates an independent monochrome virtual display and a
  detached display shell. Its logical dimensions match the board's main
  display (for example 384x288 on the Freenove PAL target). A headless board
  uses the historical 400x300 fallback. Apps launched from that shell stay on
  `web0` and do not replace the foreground app on a physical display.

With no target argument, `displayd` mirrors `display0` when it exists and
otherwise creates `web0`. The latter makes the same command useful on headless
PSRAM-equipped boards.

Usage:

```text
job start displayd [target]
job stop displayd
job status displayd
```

Example:

```text
wifi on
job start displayd
job start displayd web0
```

Starting the job prints a random six-digit access code. Open
`http://<device>/display`, enter that code, and click the displayed image
before typing. The browser frontend polls native RGB565 frames on color
targets and 1-bit U8g2 frames on monochrome targets up to twenty times per
second. It performs pixel rotation in the browser instead of
the HTTP server task. It sends bounded key input through the scheduler.
`Ctrl+]` remains the application-exit key.

API:

```text
GET  /api/displays
GET  /api/displays/<target>/frame.pbm
GET  /api/displays/<target>/frame.raw
GET  /api/displays/<target>/frame.rgb565
POST /api/displays/<target>/input
```

All API requests require `Authorization: Bearer <code>`. The access code is
never accepted in the URL. The built-in frontend itself is public so that it
can prompt for the code, but keeps the supplied code only in page memory.

Display metadata reports `pixel_format` as `rgb565` or `mono1` and supplies
`frame_rgb565` for color targets. The color endpoint contains native row-major
RGB565 pixels, most-significant byte first, with `native_width * 2` bytes per
row and the metadata's rotation. `frame.raw` retains the native 1-bit U8g2
layout; `frame.pbm` remains a monochrome image. Color targets provide both
formats. The color endpoint returns 404 for monochrome targets.

Notes:

- `web0` is registered as `source=virtual`, `driver=framebuffer` while the job
  is running. Its framebuffer and session exist independently of whether a
  browser is connected.
- `displayd` creates and owns the `web0` shell session itself and prints its
  session ID. Do not run `session create shell web0` afterward; the target is
  already attached to the browser-controlled shell.
- `Ctrl+]` exits a foreground app on `web0` and returns to its detached shell.
  The physical foreground session is unaffected.
- The physical mirror reuses the active U8g2 display and does not create
  another display session. The built-in display shell is registered as the
  session attached to `display0`.
- A consistent frame snapshot and raw transmit buffer are held in PSRAM while
  the job runs. Color targets keep an additional RGB565 snapshot and use a
  color-sized transmit buffer: together these require four bytes per native
  pixel plus the 1-bit snapshot. For the monochrome 400x300 Waveshare display,
  the snapshot and transmit buffer consume 30,400 bytes in total. `web0`
  additionally owns a board-sized U8g2
  framebuffer (15,200 bytes on Waveshare, 13,824 bytes on the 384x288 PAL
  target). HTTP transmission never holds a display or registry lock.
- Indexed graphics and full-screen, unscaled RGB565 frames, including Jet
  scenes, preserve their original colors in the browser. The monochrome
  endpoints provide dithered snapshots. Partial or scaled RGB565 raster updates
  are not mirrored.
- The snapshot is copied into the transmit buffer and released before network
  I/O, so a slow browser cannot prevent newer display frames from being
  published.
- The browser uses `frame.rgb565` for color targets and `frame.raw` for
  monochrome targets. The PBM endpoint remains available for external clients.
- If a browser is still reading a frame when the display presents again, that
  publication is skipped rather than blocking the display.
- Input is queued by the HTTP task and dispatched only by the normal SolarOS
  scheduler. Both physical and virtual targets receive it through their active
  target-addressed session; browser control does not depend on the device's
  globally foreground session.
- The server is plain HTTP. The six-digit code provides convenient access
  control on a trusted Wi-Fi network but does not encrypt frames or input and
  is not intended for exposure to an untrusted network.
- `displayd` and `httpd` share one HTTP server and may run simultaneously.

## httpd

Static HTTP file server for a folder on mounted storage.

Usage:

```text
job start httpd <folder>
job stop httpd
job status httpd
```

Example:

```text
job start httpd /www
```

Notes:

- Relative paths resolve under the default storage mount.
- The server uses the ESP-IDF default HTTP port.
- It shares the service-owned HTTP server with `displayd`.
- It serves files and simple directory listings.
- MIME types are provided for common text, image, audio, JSON, JavaScript, and
  CSS files.

## ftpd

Unencrypted FTP file server for one exported folder. The job supports one
client at a time and passive IPv4 data connections.

Usage:

```text
job start ftpd <folder> [port] [--user USER --password PASSWORD]
job stop ftpd
job status ftpd
```

Examples:

```text
job start ftpd /shared
job start ftpd /shared 2121 --user solaros --password local-secret
```

Notes:

- The default port is `21`.
- Login is anonymous by default. Anonymous clients use `anonymous` or `ftp` as
  the username; the supplied password is ignored.
- `--user` and `--password` must be supplied together. They provide plaintext
  access control, not encryption. The password also remains in local shell
  history. Use the daemon only on a trusted network.
- FTP `/` is the exported folder. Normalized paths cannot walk above that
  folder, and the export root itself cannot be deleted, replaced, or renamed.
- Supported operations include directory listing, download, upload, create and
  remove directory, delete, rename, size, current directory, and passive-mode
  negotiation. Active mode and TLS are not supported.

## log

Runtime SolarOS log follower. It mirrors log entries to a byte-stream port or
appends them to a file.

Usage:

```text
job start log <port> [error|warn|info|debug]
job start log file <path> [error|warn|info|debug]
job stop log
job status log
```

Examples:

```text
job start log cdc0
job start log uart0 debug
job start log file /.shell/log info
```

Notes:

- Port targets use CRLF line endings.
- File targets use LF line endings and are flushed periodically.
- If no level is specified, the current runtime log level is used.
- The log job starts from the latest entry, so it follows new logs rather than
  dumping the whole ring.

## telnetd

Remote Telnet shell server. The listener is a background job; each accepted
connection is attached to its own normal SolarOS port-shell session.

Usage:

```text
job start telnetd [port] [--password password]
job stop telnetd
job status telnetd
```

Examples:

```text
job start telnetd
job start telnetd 2323 --password local-secret
```

Notes:

- The default port is `23`.
- One remote client is supported at a time. Additional clients receive a busy
  response and are disconnected.
- Telnet terminal-type and window-size negotiation select the terminal profile
  and update the shell dimensions.
- Interactive line edits use the shared port shell's coalesced redraws, so the
  cursor does not visibly jump to the prompt while typing.
- While a client is attached, telnetd holds a low-latency Wi-Fi lease that
  disables modem sleep. Disconnecting restores the normal Wi-Fi power-save
  policy.
- Disconnecting closes the child shell session and releases any foreground app
  or resource it owns.
- Remote sessions do not run `/.shell/startup`.
- Telnet is unencrypted. The optional password limits access but is also sent
  over the network in plaintext, and the start command remains in the local
  shell history. Use this service only on a trusted network.

## ntp-sync

Network time synchronization job. It updates the SolarOS wall clock from NTP
and also updates the hardware RTC when the board provides one.

Usage:

```text
job start ntp-sync [once] [interval-sec] [server]
job stop ntp-sync
job status ntp-sync
```

Defaults:

| Setting | Value |
| --- | --- |
| Interval | `60` seconds |
| Server | `pool.ntp.org` |

Examples:

```text
job start ntp-sync once
job start ntp-sync 300 time.cloudflare.com
job start ntp-sync once 60 pool.ntp.org
```

Notes:

- Wi-Fi must be connected before sync can succeed.
- In `once` mode, the job retries at the interval until the first successful
  sync, then stops itself.
- Without `once`, it keeps syncing periodically.

## email-sync

Receive-only IMAPS mailbox polling job. It fetches mail into the provider-local
`email` app and publishes each new message to the universal inbox.

Usage:

```text
job start email-sync [interval-sec] [once]
job stop email-sync
job status email-sync
```

The default interval is 300 seconds; accepted values are 30 through 86400
seconds. `once` stops the job after one attempt. The account must be configured
first:

```text
wifi on
email configure imaps://imap.example.com user@example.com app-password INBOX
job start email-sync 300
```

To start polling after each reboot, add the following after `wifi on` in
`/.shell/startup`:

```text
job start email-sync 300
```

Notes:

- TLS certificate validation is mandatory; plaintext IMAP is not accepted.
- The first synchronization imports up to the newest eight messages. Later
  polls process new UIDs in batches of eight, so a busy mailbox catches up over
  successive intervals without overflowing the response buffer.
- The provider-local list keeps 32 messages in volatile memory. Universal inbox
  notifications use the mailbox as topic, the `From` header as sender, and the
  subject as title.
- Body previews are best effort. Full MIME decoding, attachments, SMTP sending,
  and server-side read-state synchronization remain future work.

## pocsag

POCSAG pager receiver job. It configures a registered packet radio for a
continuous POCSAG byte stream, frames successive 64-byte batches, filters pages
to one receiver identity code (RIC), decodes alphanumeric or numeric payloads,
and publishes completed messages to the universal inbox.

Usage:

```text
job start pocsag <radio> <frequency-hz> <baud> <ric> [alpha|numeric] [normal|inverted]
job stop pocsag
job status pocsag
pocsag status
pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha|numeric] [normal|inverted] [function]
```

Example:

```text
job start pocsag radio 448425000 1200 1841525 alpha
inbox list unread

job stop pocsag
pocsag send radio 448425000 1200 1841525 "SolarOS calling" alpha inverted
```

Notes:

- The decoder validates POCSAG parity and BCH and corrects up to two erroneous
  bits per codeword.
- Messages may continue across batch boundaries; the receiver follows the sync
  words between batches until the page is complete.
- Identical repeated pages received within 30 seconds produce one inbox entry.
- The default FSK polarity is `normal`; retry with `inverted` if batches remain
  at zero while the transmitter is active.
- `pocsag status` shows batch/message counts, corrections, receive errors, and
  the RSSI of the most recent batch.
- Stopping the job restores the radio configuration and state that were active
  when it started.
- Sending supports messages spanning multiple batches and restores the radio's
  previous configuration afterward. A receiver job using the same half-duplex
  radio must be stopped first.

## meshcore

Non-forwarding MeshCore companion provider for Contacts and Messages.

Usage:

```text
job start meshcore <radio> <profile>
job stop meshcore
job status meshcore
meshcore status
```

Example:

```text
job start meshcore radio0 meshcore-eu868
meshcore advert flood
chat
```

The job requires PSRAM and a packet-radio expansion capability. It claims the
radio, applies the explicit regional profile, sends one zero-hop startup
advert, and continuously handles adverts, direct messages, ACKs, and group
messages. Its complete protocol context is allocated as external-required
PSRAM; the 7168-byte worker stack remains internal and its minimum watermark is
reported by `meshcore status`.

Stopping restores the previous radio configuration and state before releasing
ownership. MeshCore and `radio-link` therefore report normal ownership
conflicts when pointed at the same radio. See [meshcore.md](meshcore.md) for
identity, trust, channel, regional-profile, and security details.

## meshcore-ble

MeshCore companion-protocol client for a separate BLE radio device. The job
connects to one explicit peer, synchronizes its contacts and channels, drains
queued messages, and carries Chat messages through the standard MeshCore
messaging provider.

Usage:

```text
job start meshcore-ble <address> <public|random|rpa_public|rpa_random> [six-digit-pin]
job status meshcore-ble
job stop meshcore-ble
```

Example:

```text
job start meshcore-ble 68:ee:8f:69:5f:35 public 123456
contacts
chat
```

The job requires BLE and PSRAM. The optional PIN requests Secure Connections
pairing; inspect `job status meshcore-ble` and confirm `encrypted=yes` and
`bonded=yes`. The PIN is retained only while the job runs, but the start command
can remain in shell history. The worker reconnects with exponential backoff and
coexists with the BLE keyboard; a concurrent keyboard connection attempt can
delay a reconnect but does not tear down either established link.

`meshcore` and `meshcore-ble` are alternative transports for the same messaging
provider and cannot run at the same time. Contacts, direct conversations, Chat,
and Inbox remain provider-neutral. Channels and the external device's radio and
identity are configured on the companion device; the `meshcore` shell command
continues to configure only SolarOS's local packet-radio implementation. See
[meshcore.md](meshcore.md) for transport and security details.

## espnow-link

ESP-NOW adapter for the transport-independent SolarOS Link service.

Usage:

```text
job start espnow-link <link> [channel=auto|1..13] [phy=normal|lr500|lr250] [inbox=off|on] [chat=off|on]
job stop espnow-link
job status espnow-link
espnow status
```

Example:

```text
job start espnow-link link0 channel=6 phy=lr500 chat=on
link send link0 broadcast "hello"
espnow peers
```

The job leases the Wi-Fi radio, creates a 250-byte-MTU Link, and moves complete
Link frames through ESP-NOW. `channel=auto` follows an active station or AP and
otherwise selects channel 6. `inbox=on` and `chat=on` have the same behavior and
mutual exclusion as `radio-link`.

`phy=normal` is the default. `phy=lr500` and `phy=lr250` enable Espressif's
proprietary 500 kbit/s or 250 kbit/s Long Range PHY for ESP-NOW peers. Every
device participating in an LR link must enable LR reception; use the same mode
at both ends for symmetric throughput. The service adds LR receive support
while it runs, applies the selected transmit rate to configured and learned
peers, and restores the previous Wi-Fi protocol selection when it stops. Use
`espnow status` to confirm the active PHY.

Incoming frames learn volatile Link-ID-to-MAC mappings; `espnow peer add`
stores a mapping in NVS for cold-start unicast. The service is bounded to 19
peers and four queued receive frames. Its queues and 6144-byte internal worker
stack exist only while the job runs; durable state is in PSRAM. ESP-NOW is
unencrypted in this release. See [link.md](link.md) for channel coexistence,
peer conflicts, payload limits, and security constraints.

## radio-link

Packet-radio adapter for the transport-independent SolarOS Link service.

Usage:

```text
job start radio-link <link> <radio> <profile> [inbox=off|on] [chat=off|on] [repeater=off|on]
job stop radio-link
job status radio-link
link status <link>
```

Example:

```text
job start radio-link link0 radio0 lora-eu868 chat=on
link send link0 broadcast "hello"
link status link0
```

The job claims the radio, applies the complete named profile, creates the Link
instance, transmits its queued frames, and continuously receives complete radio
packets. The Link service validates its own CRC, suppresses duplicates, replies
to requested unicast acknowledgements, and retains accepted messages in a
bounded queue. `inbox=on` additionally publishes accepted text messages to the
universal inbox. `chat=on` instead registers Link as a messaging provider,
creates a broadcast conversation, discovers source IDs as Contacts, and
supports direct and broadcast text through Chat. Both options are off by
default and cannot be enabled together.

`repeater=on` turns the same packet-radio adapter into a one-hop SolarOS Link
range extender. It retransmits valid frames for other destinations and
broadcasts, including acknowledgements and virtual-stream packets, while
preserving the original Link identities. A relayed-frame marker, randomized
delay, recent-frame suppression, and a bounded four-frame queue prevent loops
and reduce collisions. It is off by default. `job status radio-link` reports
forwarded, suppressed, queued, queue-drop, and invalid-frame counters.

Repeater mode can run together with local Chat and peer-bound Link streams. For
example, use `job start radio-link link0 radio0 lora-eu868 chat=on repeater=on`
to join Chat while extending its range. Direct frames for the local Link ID are
consumed locally, broadcasts are consumed and repeated, and frames for other
Link IDs are repeated without local delivery. A local stream can be created
with `link stream create link0 vser0 <peer-id>` while repeating stream traffic
between other devices.

Stopping restores the radio configuration and state that existed before the
job started. Mutating direct radio operations are rejected while the radio is
owned by the job. See [link.md](link.md) for commands, frame layout, IDs,
queue limits, transport MTUs, and version-one exclusions.

## pppd

PPP over any bidirectional SolarOS byte-stream port. A physical UART, USB CDC,
or a virtual port carried by a packet-radio link can provide the bytes; the PPP
negotiation mode and the interface's routing role are configured separately.

Usage:

```text
job start pppd <port> [baud|baud=rate]
    [role=downstream|uplink|peer] [mode=passive|active]
    [local=address] [peer=address] [dns=auto|none|address]
    [priority=0..255]
job stop pppd
job status pppd
```

Defaults:

| Setting | Value |
| --- | --- |
| Baud | Keep the port's current rate |
| Role | `downstream` |
| Mode | `passive` for downstream; `active` otherwise |
| Local address | `192.168.8.1` for downstream |
| Peer address | `192.168.8.2` for downstream |
| DNS | `auto`, copied from the preferred SolarOS uplink |
| Uplink priority | `90` |

Routing roles have these effects:

- `downstream` enables NAPT on the PPP interface. A connected computer can use
  whichever Wi-Fi, cellular, or WireGuard routes SolarOS currently selects.
- `uplink` registers `ppp-<port>` as a base network path. It participates in
  the same persistent priority selection as Wi-Fi and cellular interfaces.
- `peer` creates only the point-to-point interface. It does not enable NAPT or
  advertise the interface as a default-route candidate.

`mode=active` starts negotiation. `mode=passive` listens for the other endpoint
to initiate it. Either mode can be combined with any routing role; the defaults
are conveniences, not a coupling between negotiation and routing.

Examples:

```text
# Retro computer dials into SolarOS and reaches its selected Internet route.
job start pppd uart0 baud=115200

# The same downstream service over native USB CDC with an explicit DNS server.
job start pppd cdc0 role=downstream dns=8.8.8.8

# Treat a virtual serial port over a radio link as a SolarOS uplink.
job start pppd vser0 role=uplink mode=active priority=120

# A routed point-to-point link without address translation.
job start pppd uart0 38400 role=peer mode=passive \
    local=10.20.0.1 peer=10.20.0.2 dns=none
```

The job exclusively claims the selected port. A baud argument is accepted only
for configurable UART ports; CDC and virtual ports retain their transport rate.
This initial service uses unauthenticated PPP. Use it on a physically trusted
link. `job status pppd` reports negotiation state, assigned addresses, NAPT,
traffic counters, reconnects, and the most recent PPP error.
While the job runs, `network` and `network status` show `ppp-<port>` with its
uplink, downstream, or peer role. Only the uplink role appears in the Network
Settings priority list.

## slip

IPv4 SLIP gateway on a byte-stream port. This is intended for retro machines,
headless boards, and serial networking experiments.
The active interface is named with the `slip-` prefix plus the selected port,
for example `slip-uart0`. It appears as a downstream NAT link in the Network
views; its traffic follows the selected SolarOS route, which can be Wi-Fi,
cellular, or WireGuard.

Usage:

```text
job start slip [port] [baud] [local-ip] [peer-ip] [netmask]
job stop slip
job status slip
```

Defaults:

| Setting | Value |
| --- | --- |
| Port | `uart0` |
| Baud | `115200` |
| Local IP | `192.168.7.1` |
| Peer IP | `192.168.7.2` |
| Netmask | `255.255.255.252` |

Examples:

```text
job start slip uart0 115200
job start slip cdc0 115200
job start slip uart0 38400 192.168.7.1 192.168.7.2 255.255.255.252
```

Notes:

- The peer should use the local IP as its gateway.
- SolarOS enables NAT on the SLIP-facing interface.
- The selected port is claimed by the SLIP job until it stops.
- `cdc0` is useful for Linux host testing; `uart0` is the natural expansion
  port path.

## graffiti

Full-screen Palm Graffiti handwriting input for boards with absolute touch and
PSRAM. The job does not draw an overlay or change the active application.

```text
job start graffiti
job status graffiti
job stop graffiti
```

The pen-down position chooses the recognition alphabet for the complete
stroke. Start in the left two-thirds of the oriented display for letters, or
in the right one-third for numbers. The boundary follows the current display
orientation. Relative pointer sources are ignored.

The recognizer implements the `$1` unistroke pipeline with Palm Graffiti 1
alphabet and numeral templates. It preserves stroke direction so a downward
vertical stroke can be `I` or `1`, while an upward vertical stroke is Shift.
The Palm editing gestures are also available:

| Stroke | Result |
| --- | --- |
| Upward vertical | Shift; repeat before a character for Caps Lock. |
| Left to right | Space. |
| Right to left | Backspace. |
| Upper right to lower left | Enter. |

While running, the job registers the `pointer-observer` resource. It observes
absolute pointer events over the whole display but does not consume them, so
the foreground application continues to receive the same press, move, and
release events. Recognized characters are published by a virtual keyboard
source named `graffiti` and therefore go to the current input focus. `job
status graffiti` reports recognized, rejected, and dropped stroke counts plus
the current case state.

## pointer-arrows

Consume one named pointer source and hold arrow keys while it moves:

```sh
job start pointer-arrows source=trackball0
job status pointer-arrows
job stop pointer-arrows
```

Set `min-vector=1..65535` to require a minimum net vector length in each
evaluation window. The default is `2`; larger values reduce sensitivity.
For example, restart T-Deck navigation with:

```sh
job stop pointer-arrows
job start pointer-arrows source=trackball0 min-vector=4
```

Length is `sqrt(dx*dx + dy*dy)` in the source's movement units. For `trackball0`,
one interrupt edge contributes one unit on its axis. Movement below the minimum
releases a held arrow and is discarded at the end of the window; it does not
accumulate across windows. The option also applies to the last partial window
when movement stops or a touch is released.

The job exclusively captures that source's pointer events, including contact
and button boundaries. They are withheld from foreground pointer consumers and
the pointer filter while the job runs. Other pointer sources remain available.
Pointer buttons do not produce keyboard keys. Release any held button or touch
before starting the job.

Movement is evaluated in fixed 80 ms windows, starting with the first movement
event. Each window that meets the minimum selects the dominant net direction
and presses that arrow. Continued movement in the same direction keeps the key
held, using the normal `setterm keyrate` repeat settings. A direction or modifier
change releases the old key before pressing the new one. Opposite movement
cancels within a window;
equal diagonals use the most recently moved axis. A zero net vector releases
the arrow. Trackball windows retain the modifiers from their last interrupt
edge. Other pointer sources use keyboard modifiers when samples are processed.

After 80 ms without a movement event, the held key is released. A short roll
that meets the minimum produces a press and release. Touch release ends an
absolute-pointer gesture immediately. Stopping the job, source loss, or a
capture reset also releases held keys and cancels repeat.

Relative pointers supply deltas directly. Absolute pointers are measured from
each contact's initial position; a new contact starts a new baseline. Only one
contact is tracked at a time. Job status reports the captured source, minimum
vector length, direction evaluations that meet it, capture resets, and
evaluation and inactivity intervals.

Stopping releases capture and discards unevaluated movement. If stopped during a
held button or contact, pointer delivery resumes after its release. Source
closure or loss of readiness stops the job and releases its resources. T-Deck
starts this job for `trackball0` before the shell and startup script run.

## gpio-keys

Maps runtime-safe GPIO inputs to SolarOS keyboard presses. The job configures
each pin as an input with its internal pull-up enabled, treats a low level as
pressed, and applies the same 25 ms debounce used by fixed board buttons. Each
debounced transition publishes a generic SolarOS key press or release. Held
keys use the system repeat rate configured by `setterm keyrate`.

Inline usage:

```text
job start gpio-keys gpio17:UP gpio2:ENTER gpio3:ESCAPE
job stop gpio-keys
job status gpio-keys
```

Configuration-file usage:

```text
job start gpio-keys --config /flash/gpio-keys.conf
```

The file contains one mapping per line. A colon or whitespace can separate the
pin and key. Empty lines and text after `#` are ignored:

```text
# navigation buttons, active low
gpio17:UP
gpio16 DOWN
gpio4:ENTER
```

Canonical pins use `gpioN`; the job also accepts the `ioN` shorthand. Key names
are case-insensitive. Canonical directional names are `UP`, `DOWN`, `LEFT`, and
`RIGHT`; aliases such as `ARROW_UP` are accepted. Other names include `ENTER`,
`SPACE`, `TAB`, `BACKSPACE`, `ESCAPE`, `HOME`, `END`, `DELETE`, `PAGE_UP`,
`PAGE_DOWN`, `F1` through `F12`, and the modified SolarOS navigation keys. A
single character maps that exact character.

The complete configuration is validated before any pin is changed. Up to 16
pins can be loaded from a file; the shell's argument limit allows up to seven
inline mappings. Duplicate pins, fixed/reserved pins, unknown keys, and busy
pins reject the complete start. Starting with a button already held does not
generate a press. The file is read only at startup; restart the job to reload
it.

While the job runs, every pin belongs to the `gpio-keys-job` expansion
attachment and has an assignment such as `key:UP` in the `io` Pins and Claims
views. Stopping the job detaches that device, resets the pins, releases their
claims, and discards queued events from this input source. The fixed ODROID-GO
button service remains independent but uses the same held-key and repeat
service.

## ps2-keyboard

Receives keyboard scan-code set 2 from an exclusive named PS/2 bus and publishes
press and release transitions through the generic SolarOS input service. This
job is a compatibility wrapper around a `ps2-keyboard` expansion attachment;
new configurations can attach the device directly.

```text
expansion bus create ps2 ps2kbd clock=gpio17 data=gpio18
job start ps2-keyboard ps2kbd
job status ps2-keyboard
job stop ps2-keyboard
```

These commands cover an expansion bus. Boards with an integrated PS/2 keyboard
declare the bus and a default expansion attachment. On TTGO VGA32 v1.4,
`keyboard0` is attached to `ps2kbd0` before the shell starts and is inspected
with `expansion devices` and `input test keyboard0`.

The bus descriptor owns the CLOCK and DATA pins as `bus:ps2kbd`; the wrapper's
`ps2-keyboard-job` attachment holds the exclusive lease. Normal and
extended keys, modifiers, navigation keys, function keys, and keypad usages are
translated to canonical USB HID identities. The configured `setterm keyboard`
layout and `setterm keyrate` repeat policy apply equally to BLE and PS/2.

The receiver validates each PS/2 frame's start bit, odd parity, and stop bit in
the GPIO clock-edge handler. Scan-code parsing and input publication run from
the normal job tick, outside interrupt context. The current driver only
receives keyboard data; it does not send LED or reset commands to the keyboard.

Use a bidirectional level shifter or another circuit that guarantees no more
than 3.3 V at the ESP32 GPIOs. ESP32 inputs are not 5 V tolerant.

## midi

Bidirectional MIDI transport on an exclusive named MIDI bus. The bus selects
an available UART controller internally; users supply only its MIDI name, TX
and RX pins, and an optional baud rate.

```text
expansion bus create midi midi0 tx=gpio1 rx=gpio2
job start midi midi0
job status midi
midi status
job stop midi
```

The default rate is the MIDI DIN rate of 31250 baud. Incoming channel voice,
system-common, and realtime messages are decoded with running-status support
and published to subscribers such as the Synth app. Outgoing messages are
queued with `midi note-on`, `midi note-off`, `midi cc`, `midi program`, or
`midi send`. Status reports RX and TX byte/message counts, unsupported parser
input, queue drops, and the last transport error.

Run `midi monitor` and move a controller to identify its mapping. The monitor
prints `CC: <channel> <controller> <value>` for control changes and
`KEY: <channel> <note> <velocity>` for note activity. Note releases use velocity
zero. The app-exit key, `Esc`, or `q` returns to the shell.

Up to 16 exact incoming MIDI CC addresses can also be registered as scalar
streams. This lets the controls job map a MIDI controller through the standard
normalized control path to any live application parameter:

```text
midi stream add 1 74
control create cutoff midi.cc.1.74 0 127
control bind cutoff parameter synth.filter.cutoff pickup=off
job start controls
synth
```

Use `midi stream list`, `midi stream remove <channel> <controller>`, and
`midi stream clear` to inspect or remove the volatile definitions. A stream is
waiting until the running MIDI job receives its first matching value.

Use a compliant electrical interface: MIDI IN requires an optoisolated
receiver and MIDI OUT requires a current-limited driver. Do not connect DIN
MIDI pins directly to ESP32 GPIOs.

## speechd

Offline text-to-speech queue. The optional job loads a PicoTTS voice from
storage into PSRAM and accepts asynchronous requests from native applications,
Python, and Lua.

```text
job start speechd /voices/en-US
say "Solar O S is ready"
say --pitch 85 --speed 120 "Solar O S is ready"
say --file /documents/announcement.txt
say --file /books/novel.txt
job status speechd
job stop speechd
```

`say --file <path>` validates and reads a plain UTF-8 text file in bounded
chunks. It keeps the shell in a foreground playback mode, but advances from
shell events so the display and progress bar refresh while speech is running.
The percentage is submitted-file progress: it advances whenever the next
bounded chunk is accepted by `speechd`. The complete file is one streaming
speech request: PicoTTS and the audio player stay open between chunks, while a
single bounded handoff slot prevents the file from being buffered in memory.
Press `Esc` or `Ctrl+C` to cancel the current speech request and stop reading.
The foreground command returns only after `speechd` confirms cancellation, so a
new `say` request cannot race the previous stream's cleanup. Cancellation
discards queued text and soft-resets PicoTTS instead of synthesizing the
unspoken remainder of the file.
There is no file-size limit because only one bounded chunk is held and submitted
at a time. Tab completion after `--file` lists filesystem paths. Ordinary `say
<text...>` remains asynchronous and returns the queued request ID.
`--pitch 50..200` and `--speed 20..500` use PicoTTS's native controls; both
default to 100 and apply to ordinary text and file playback.

The voice directory must contain `ta.bin` and `sg.bin`. No voice blobs are
compiled into `firmware.bin`. The repository's top-level `picotts_voices/`
directory contains ready-to-copy voices for `en-GB`, `en-US`, `de-DE`, `es-ES`,
`fr-FR`, and `it-IT`. Builds also mirror them below
`.pio/build/<environment>/picotts_voices/`. Copy one or more complete locale
directories to SD or flash storage, then select the language when starting the
job:

```text
job start speechd /voices/de-DE
```

Restarting `speechd` with another directory changes the language at runtime.
The directory name is used as the voice label in `job status speechd`; PicoTTS
validates the TA and SG resource contents while starting. Missing, truncated,
or incompatible resources make the job fail instead of accepting requests.

The job acquires the selected audio output only while an utterance is active
and releases it between requests. Normal requests wait for another audio owner;
callers can mark disposable notifications `drop_if_busy`.

The queue holds eight requests of at most 512 UTF-8 bytes. Request IDs support
polling and cancellation through `solaros.speech`. Stopping the job cancels
pending speech and releases the roughly 1.1 MiB PicoTTS engine allocation plus
the selected TA and SG buffers.

## sump

SUMP-compatible logic analyzer server on `cdc0`. It claims the CDC port and
uses the shared logic analyzer service for acquisition. PulseView and sigrok
can connect with the OpenBench Logic Sniffer/SUMP serial driver.

Usage:

```text
job start sump [pin ...]
job start sump [pin[,pin...]]
job stop sump
job status sump
```

Examples:

```text
job start sump
job start sump 1 2 3 17
job start sump 1,2,3,17
```

If no pins are supplied, the job uses up to eight runtime-safe GPIOs from the
active board profile. Host commands select the sample rate and capture size.
The current implementation supports 10 kHz to 2 MHz requests and up to 32768
one-byte samples; the status recorded with each capture reports the measured
effective rate.

Notes:

- The job requires both CDC and runtime-safe GPIO capabilities.
- `cdc0` cannot be used by a port shell, logger, bridge, or another job while
  SUMP is active. Start SUMP from the display shell, SSH, or another port after
  stopping any shell attached to `cdc0`.
- Basic trigger commands are accepted for host compatibility, but this first
  implementation captures immediately instead of waiting for a trigger.
- Captures remain available to the `logic` app after the job stops.

## Quick reference

Use `jobs` to inspect state and memory requirements, `job NAME start` to launch
a worker, `job NAME status` for details, and `job NAME stop` to release it.
Jobs run in the background and may claim ports or hardware resources; this page
documents every installed job's arguments and ownership rules.
