+++
id = "link"
title = "SolarOS Link"
section = "service"
summary = "Packet messaging and reliable virtual serial ports over packet radio or ESP-NOW"
aliases = ["radio-link", "espnow-link", "link.protocol"]
keywords = "link packet radio repeater relay range extender esp-now espnow wifi text binary stream virtual serial port shell acknowledgement retry broadcast queue protocol crc duplicate"
packages_any = ["service_link", "job_radio_link", "job_espnow_link"]
+++
# SolarOS Link

SolarOS Link is a small transport-independent message layer for packet-sized
connections. The Link service owns framing, protocol CRC, sequence numbers,
acknowledgements, duplicate suppression, and bounded receive/transmit queues.
A transport adapter moves complete Link frames over a specific medium.

The base packet layer deliberately does not provide routing, fragmentation,
encryption, or mesh forwarding. The packet-radio adapter can optionally repeat
each frame by one hop. One text or binary message must fit the selected
transport MTU. The optional Link stream layer segments a byte stream into those
packets and adds ordered delivery, retransmission, and backpressure.

## Radio Quick Start

Attach a packet radio, then start the adapter with a complete radio profile:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
job start radio-link link0 radio0 lora-eu868
link status link0
link send link0 broadcast "hello"
```

To copy received text messages into the universal inbox:

```text
job start radio-link link0 radio0 lora-eu868 inbox=on
```

`inbox=off` is the default. Accepted text and binary messages remain available
through the bounded Link receive queue in either mode; `inbox=on` additionally
publishes accepted text messages to the inbox. When this diagnostic queue is
full, Link evicts its oldest queued copy while continuing to deliver the newest
accepted frame to Inbox or Chat. Read one queued message with:

```text
link receive link0
link receive link0 1000
```

The optional timeout is in milliseconds and is limited to 1000 in the shell.

To use Link text in the unified Chat and Messages interfaces instead:

```text
job start radio-link link0 radio0 lora-eu868 chat=on
chat
```

Chat shows a `link` provider section with a `link0 broadcast` conversation.
Each received 32-bit source ID creates a discovered Contact and direct
conversation. Rename, trust, or block it with the normal `contacts` commands.
The Chat projection observes accepted frames without consuming the Link receive
queue, so `link receive` remains available as a bounded recent-frame diagnostic.
Its capacity does not limit Chat delivery. `chat=on` and `inbox=on` are mutually
exclusive because generic messaging already publishes received Chat messages to
Inbox.

## ESP-NOW Quick Start

Start the ESP-NOW adapter on two SolarOS devices using the same channel:

```text
job start espnow-link link0 channel=6 phy=lr500
link status link0
link send link0 broadcast "hello"
```

The default `channel=auto` follows an active station or access-point channel.
When neither exists, it starts on channel 6. A fixed channel prevents a new
station connection or an AP on another channel. Wi-Fi scanning is rejected
while ESP-NOW is active because a scan leaves the transport channel. `wifi off`
stops station and AP networking but retains the radio while `espnow-link` owns
its connectionless lease.

The default `phy=normal` uses the normal ESP-NOW PHY selection. Use
`phy=lr500` or `phy=lr250` on every participating SolarOS device to select
Espressif's proprietary 500 kbit/s or 250 kbit/s Long Range PHY. LR mode
trades throughput for receive sensitivity and range. SolarOS enables LR receive
support for the lifetime of the job, assigns the selected rate to configured
and newly learned peers, and restores the previous Wi-Fi protocol selection on
stop. `espnow status` reports the active PHY.

An accepted incoming frame learns its Link source-ID-to-MAC mapping for the
current boot. This permits unicast replies after the peer has sent a frame.
Add a persistent mapping when this device must initiate unicast after boot:

```text
espnow peers
espnow peer add 0x12345678 24:6f:28:11:22:33
espnow peer remove 0x12345678
```

There are 19 peer slots. Configured peers are stored in NVS; learned peers are
volatile and the oldest learned entry is evicted when necessary. SolarOS
rejects a learned mapping that conflicts with an existing source ID or MAC
instead of silently redirecting traffic.

## Commands

| Command | Description |
| --- | --- |
| `link status\|list` | List active Link instances and their queue/protocol counters. |
| `link status <link>` | Show one Link instance, local ID, MTU, queue depths, ACK state, duplicates, CRC errors, and drops. |
| `link send <link> <broadcast\|destination-id> <text>` | Queue one text message. Unicast requests an acknowledgement. |
| `link send-binary <link> <broadcast\|destination-id> <byte...>` | Queue one binary message from decimal or `0x` byte values. |
| `link receive <link> [timeout-ms]` | Remove and print the oldest accepted text or binary message. |
| `link stream list` | List Link-backed virtual serial ports. |
| `link stream status [port]` | Show peer, connection, MTU, queues, traffic, retry, reconnect, drop, and error state. |
| `link stream create <link> <port> <peer-id>` | Register a peer-bound Link stream as a normal bidirectional SolarOS port. |
| `link stream remove <port>` | Remove an unclaimed Link stream port. |

Destination IDs accept decimal or `0x` notation. `broadcast` is the reserved
destination `0xffffffff`. `link status` prints this device's stable 32-bit
local ID as hexadecimal; it is derived from the ESP32 base MAC.

The displayed frame MTU comes from the active transport and radio profile. The
payload limit is the MTU minus the 12-byte header and 2-byte protocol CRC. For
example, the 255-byte LoRa packet profile carries at most 241 Link payload
bytes, a 64-byte FSK profile carries at most 50, and the 250-byte ESP-NOW
transport carries at most 236.

## radio-link Job

Usage:

```text
job start radio-link <link> <radio> <profile> [inbox=off|on] [chat=off|on] [repeater=off|on]
job status radio-link
job stop radio-link
```

The job claims the radio as `job:radio-link`, applies the named radio profile,
creates the Link instance, and continuously alternates queued transmission and
packet reception. While it runs, mutating `radio config`, `radio state`,
`radio send`, `radio recv`, and profile-apply operations are rejected; read-only
radio status remains available and shows the owner.

Stopping the job destroys its Link queues, restores the radio configuration and
state that existed at startup, and releases the radio. Only one instance of the
`radio-link` job can run at a time, matching the normal SolarOS job registry.

`repeater=on` makes the device a one-hop store-and-forward range extender on
the selected radio and profile. It repeats valid Link frames that are not
addressed to the local device, including broadcasts, acknowledgements, and
virtual-stream traffic. The forwarded copy preserves the original source,
destination, sequence, type, and payload. A Link header flag marks that copy as
already relayed, so another repeater does not forward it again.

Each repeater waits for a short randomized interval before transmission. If it
hears another repeater forward the same frame first, it suppresses its pending
copy. A direct acknowledgement also cancels the corresponding pending text or
binary frame while the acknowledgement itself remains eligible for repeating
back toward the sender. Recent-frame caching and a bounded four-frame relay
queue prevent loops and unbounded memory use. Inspect its counters with `job
status radio-link`:

```text
job start radio-link link0 radio0 lora-eu868 repeater=on
job status radio-link
```

Repeating doubles the packet-radio airtime used for traffic that crosses the
repeater and increases acknowledgement latency. All devices must use the same
frequency, modulation, packet profile, and Link framing. Repeater mode does not
add encryption or authentication; it forwards any structurally valid Link
frame received on that profile. It is off by default.

The Link queues have four entries each and use PSRAM when available. They are
created only when a Link starts, so the compiled service has no idle queue
allocation. Receive-queue overflow evicts the oldest queued copy and increments
the drop counter rather than rejecting the accepted frame or growing without
bound. Transmit-queue overflow still rejects a new send.

With `chat=on`, outgoing broadcast text becomes `sent` after the radio accepts
the frame. Direct text remains `sending` until the matching Link
acknowledgement arrives and becomes `failed` after ten seconds without one.
There is no automatic retry. Text that exceeds the active Link payload MTU
fails with the exact byte limit. Link contacts remain discovered until
explicitly trusted, and Link messages carry no encrypted or transport-secured
security flag.
Each messaging-adapter session scopes Link source/sequence identities with a
fresh local epoch, so restarting `radio-link` does not make new packets collide
with retained messages from the previous sequence cycle.

## espnow-link Job

Usage:

```text
job start espnow-link <link> [channel=auto|1..13] [phy=normal|lr500|lr250] [inbox=off|on] [chat=off|on]
job status espnow-link
job stop espnow-link
```

The job acquires the Wi-Fi radio through the connectionless lease, initializes
ESP-NOW, creates a Link instance with a 250-byte frame MTU, and moves complete
frames between the two services. Inbox and Chat behavior is the same as for
`radio-link`; both options default to off and cannot be enabled together. The
PHY defaults to `normal`; every LR peer must enable LR reception. Use the same
`lr500` or `lr250` mode at both ends for symmetric throughput.

The transport state and peer table live in PSRAM. Its bounded four-frame receive
queue, two-entry send-completion queue, and 6144-byte time-critical worker stack
are allocated only while the job runs and released when it stops. A stopped
compiled service therefore has no queues or worker stack reserved for a future
ESP-NOW session.

`radio-link` and `espnow-link` can run at the same time with different Link
names. Only one can use `chat=on` because the Link messaging projection is a
single provider. Direct Link queues and `inbox=on` do not share that provider.

ESP-NOW frames are unencrypted in this release. Link IDs and learned MAC
mappings are not cryptographic identities, so peers can be spoofed. Do not
expose a privileged Link stream over an untrusted ESP-NOW channel.

## Version 1 Frame

All multi-byte values use network byte order:

| Offset | Size | Field |
| --- | ---: | --- |
| 0 | 1 | Version in the high nibble, flags in the low nibble |
| 1 | 1 | Message type |
| 2 | 2 | Sequence number |
| 4 | 4 | Source ID |
| 8 | 4 | Destination ID |
| 12 | variable | Payload |
| final 2 | 2 | CRC-16/CCITT-FALSE over header and payload |

Message types are `1` text, `2` binary, `3` acknowledgement, and `4` stream.
Unicast text and binary frames set the acknowledgement-requested flag. An
acknowledgement has no payload, swaps source/destination, and echoes the
acknowledged sequence number. Broadcast frames never request acknowledgements,
avoiding an ACK storm. Stream frames use their own ordered acknowledgement and
retry protocol and therefore do not request the base Link acknowledgement.

The receiver remembers the 12 most recent source/sequence/type tuples. A
duplicate is not delivered or copied to the inbox, but is acknowledged again
when requested so a sender can recover from a lost ACK. Up to eight outstanding
unicast sequence/destination pairs are tracked for status; version one does not
automatically retransmit an unacknowledged frame.

The protocol CRC is present even when the transport also supplies a hardware
CRC. This keeps integrity checking consistent across packet radio, serial,
infrared, or future transports.

## Serial byte bridge

The existing `bridge` job can connect one bidirectional byte-stream port to an
active Link:

```text
job start radio-link link0 radio0 lora-eu868
job start bridge uart0 link0 broadcast
```

Serial input is sent as binary Link messages capped to the active payload MTU.
Received text and binary payloads are written back to the serial stream without
separators. Replace `broadcast` with a decimal or `0x` destination ID for
acknowledged unicast. The bridge claims the serial port and consumes the Link
receive queue until stopped.

This is a bounded, best-effort stream adapter rather than Link fragmentation or
flow control. Packet radio can be much slower than a serial producer, so
sustained input can overrun the serial driver or the four-entry Link queue.

## Virtual serial ports

For an ordered interactive stream, create the same peer-bound Link stream on
both devices. The stream name becomes a normal SolarOS byte-stream port and is
shown by both `link stream list` and `port list`:

```text
# Device A; Device B reports local ID 0xde63d29e
job start radio-link link0 radio0 lora-eu868
link stream create link0 vser0 0xde63d29e

# Device B; Device A reports local ID 0x7b1fdb02
job start radio-link link0 radio0 lora-eu868
link stream create link0 vser0 0x7b1fdb02
```

The port is initially `closed`. It starts negotiating when a normal SolarOS
consumer claims it. To expose a Waveshare shell over the radio:

```text
# Waveshare
session create shell vser0 --term dumb

# DevKit ground station with display and keyboard
com vser0

# Or bridge a Linux USB shell through the headless DevKit
job start bridge cdc0 vser0
```

Claiming the virtual port with `com`, `session`, or `bridge` opens the stream
automatically; there is no separate stream-open command. Linux can open the
DevKit USB CDC serial device after starting the bridge and interact with the
Waveshare's normal port shell. On a headless DevKit, `uart0` remains available
for local administration while `cdc0` is owned by the bridge. `--term dumb`
avoids expensive terminal probes and escape traffic; VT100 remains available
when the radio profile has enough throughput.

Each direction uses a fresh random session epoch whenever the port opens,
16-bit byte-frame sequence numbers, ordered stop-and-wait delivery, piggybacked
stream acknowledgements, and jittered retransmission after 0.8 to 1.2 seconds.
Stream data is split to the active Link MTU and reassembled into a 2048-byte RX
queue. A separate
2048-byte TX queue applies backpressure to the shell or bridge instead of
silently overflowing the four-entry packet queue. Fifteen-second open frames
also detect a disconnected or restarted peer.

Stream protocol v2 carries an acknowledgement field in every frame.
Pending acknowledgements are piggybacked on outbound data, and a standalone ACK
is delayed by 40 milliseconds to give an interactive port shell time to queue
its echo first. A normal keystroke and its echo therefore need two radio frames
on the fast path instead of waiting for a separate ACK between them.

Stream packets are dispatched before the normal Link receive queue, so they do
not appear in `link receive` and cannot evict Chat or diagnostic packet copies.
Only packets whose Link source matches the configured peer ID enter the virtual
port. The peer binding prevents accidental cross-talk, but Link IDs can be
spoofed: Link streams are not encrypted or cryptographically authenticated.
Do not expose a privileged shell over an untrusted radio channel.

Close every shell or bridge that owns the port before removing it:

```text
session close <id>
link stream remove vser0
```

Stopping and restarting `radio-link` or `espnow-link` with the same Link name
leaves the virtual port registered. Buffered users see a disconnected stream
until the transport returns; the stream epochs then resynchronize without
reusing stale bytes.

## Quick reference

Start a packet-radio link with `job start radio-link link0 radio0
lora-eu868 [inbox=off|on] [chat=off|on] [repeater=off|on]`. Use `link status link0`, `link send
link0 broadcast "text"`, and `link receive link0`. Use `chat=on` for unified
Link broadcast/direct conversations and discovered Contacts. Use `link stream
create link0 vser0 PEER_ID` when a reliable virtual serial port is required.
For ESP-NOW, use `job start espnow-link link0 [channel=auto|1..13] [phy=normal|lr500|lr250]` and configure
cold-start unicast peers with `espnow peer add`.
Unicast packet messages request acknowledgements. The base packet layer has no
routing, fragmentation, encryption, mesh forwarding, or automatic
retransmission; `radio-link repeater=on` adds one-hop packet repetition, and the
peer-bound stream layer adds segmentation, ordering, retransmission, and
backpressure for byte-stream consumers.
