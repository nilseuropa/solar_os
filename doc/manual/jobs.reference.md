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
SLIP, chatd, or NTP sync. Foreground applications are documented in
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

`jobs` is intentionally compact so it fits on the built-in 65-column display
terminal. It shows job name, state, declared worker-stack requirement, kind,
event source, tick count, and resource count. Running rows are bold. Use
`job status <name>` for the summary, owner string, worker-stack placement, last
error, effective tick interval/deadline, runtime duration statistics, deadline
misses, and claimed resources. Compact timing lines use `interval/deadline` in
milliseconds, `n` for dispatches, `us=last/max`, and `miss` for deadline misses.

## Job Control

| Command | Description |
| --- | --- |
| `jobs` | List registered jobs in compact form. |
| `job status [name]` | Show all jobs, or one named job with details. |
| `job start <name> [args...]` | Start a job with optional arguments. |
| `job stop <name>` | Stop a running job. |

Only one instance of each job name can run at a time. Starting a running job
first stops the previous instance, then starts it again with the new arguments.

Jobs have stable owner strings in the form `job:<name>`. Jobs that claim ports,
files, streams, or network listeners publish those resources through the job
status model. This keeps port/resource conflict messages readable and avoids
job-specific inspection code in the shell.

Job states distinguish a deferred launch from a completed launch attempt:

- `waiting` means SolarOS retained the start request because admitting the
  declared worker stack would consume the internal-memory reserve. The job is
  not running yet; the scheduler retries it automatically as memory becomes
  available.
- `failed` means SolarOS attempted the job's start callback and it returned a
  terminal error. `job status <name>` prints that error. A `no memory` failure
  can still occur when a job or library needs dynamic memory beyond its
  declared worker stack; that memory is not predictable from the descriptor.
- `running` means start completed successfully. `stopped` means there is no
  active or retained launch request.

The `STACK` column is the declared worker-stack admission requirement in bytes,
not a worst-case total-memory estimate. `-` means that the job declares no
dedicated worker stack; it may still allocate dynamic buffers or use a shared
service. Compare this column with the internal figures from `mem policy`.
For an internal-stack background job, admission currently requires internal
free memory of at least `STACK + 1 KiB overhead + 32 KiB reserve`, and an
internal largest block of at least `STACK + 1 KiB overhead`. Treat a manual
check as a snapshot: other work can allocate memory before `job start`, which
is why SolarOS retains and retries a launch that loses that race.

Jobs that use byte-stream ports claim those ports while running. If a port is
already owned, SolarOS reports the owner, for example `job log owns cdc0`.
Radio listeners expose their radio as a custom job resource.

GPIO-key mappings claim each selected pin through the same resource model. The
`io` application therefore shows assignments such as `key:UP`, their
`gpio-keys-job` attachment owner, and the board's canonical pin policy without
special GPIO-key display logic.

Tick intervals and execution-time deadlines are declared by each event-driven
job. A zero descriptor value selects the runtime default. Deadline misses do
not forcibly terminate a cooperative handler; SolarOS counts them, records the
last and maximum duration, and emits rate-limited warnings. The DAQ and log
handlers only enqueue work, so their stream, filesystem, and port I/O runs in
isolated worker tasks instead of the display scheduler.

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

Jobs can be started from the normal startup script:

```text
/.shell/startup
```

Example:

```text
wifi on
job start ntp-sync once
job start batmon 60
```

## batmon

Battery monitor. It periodically samples battery voltage, maintains a smoothed
trend, estimates power state, and can request light sleep when the configured
minimum voltage is reached.

Usage:

```text
job start batmon [interval-sec]
job stop batmon
job status batmon
```

Defaults:

| Setting | Value |
| --- | --- |
| Interval | `60` seconds |

Battery limits are configured with the `battery` shell command:

```text
battery capacity <mAh>
battery min_voltage <volts>
battery max_voltage <volts>
```

Notes:

- Discharging trend means battery power.
- Charging trend means external power.
- Voltage above `max_voltage` is a fast external-power shortcut.
- Three consecutive samples at or below `min_voltage` while on battery request
  light sleep.

Example:

```text
job start batmon 60
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
job start chatd [port] [token] [--history path]
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
before typing. The browser frontend polls the native 1-bit U8g2 frame up to
twenty times per second and performs pixel rotation in the browser instead of
the HTTP server task. It sends bounded key input through the scheduler.
`Ctrl+]` remains the application-exit key.

API:

```text
GET  /api/displays
GET  /api/displays/<target>/frame.pbm
GET  /api/displays/<target>/frame.raw
POST /api/displays/<target>/input
```

All API requests require `Authorization: Bearer <code>`. The access code is
never accepted in the URL. The built-in frontend itself is public so that it
can prompt for the code, but keeps the supplied code only in page memory.

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
- A consistent 1-bit frame snapshot and same-sized raw transmit buffer are held
  in PSRAM while the job runs. For the 400x300 Waveshare display they consume
  30,400 bytes in total. `web0` additionally owns a board-sized U8g2
  framebuffer (15,200 bytes on Waveshare, 13,824 bytes on the 384x288 PAL
  target). HTTP transmission never holds a display or registry lock.
- The snapshot is copied into the transmit buffer and released before network
  I/O, so a slow browser cannot prevent newer display frames from being
  published.
- The browser uses `frame.raw` to avoid per-pixel PBM conversion on the ESP32.
  The PBM endpoint remains available for simple external clients.
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
PSRAM; the 6144-byte worker stack remains internal and its minimum watermark is
reported by `meshcore status`.

Stopping restores the previous radio configuration and state before releasing
ownership. MeshCore and `radio-link` therefore report normal ownership
conflicts when pointed at the same radio. See [meshcore.md](meshcore.md) for
identity, trust, channel, regional-profile, and security details.

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
job start radio-link <link> <radio> <profile> [inbox=off|on] [chat=off|on]
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

Stopping restores the radio configuration and state that existed before the
job started. Mutating direct radio operations are rejected while the radio is
owned by the job. See [link.md](link.md) for commands, frame layout, IDs,
queue limits, transport MTUs, and version-one exclusions.

## slip

IPv4 SLIP gateway on a byte-stream port. This is intended for retro machines,
headless boards, and serial networking experiments.

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
