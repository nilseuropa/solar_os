+++
id = "meshcore"
title = "MeshCore companion messaging"
section = "service"
summary = "Secure MeshCore messaging over a local packet radio or BLE companion device"
aliases = ["meshcore.job", "meshcore.radio"]
keywords = "meshcore lora radio ble companion identity advert contacts direct group public psk trust stream serial com shell bridge repeater"
packages_any = ["service_meshcore", "job_meshcore", "service_meshcore_ble", "job_meshcore_ble"]
+++
# MeshCore companion messaging

SolarOS exposes MeshCore through two background jobs. `meshcore`
implements a non-forwarding companion node on a locally attached packet radio.
`meshcore-ble` uses the companion protocol to connect to a separate MeshCore
device and lets that device own the radio, identity, contact table, and channel
configuration. Both publish through the provider-neutral Contacts and Messages
services, so Contacts, Chat, and Inbox use the same interfaces. Both jobs can run
together. Each maintains its configured instances: separate local radios or
separate BLE companions have independent identities, channels, routing, and
connection state. A physical radio or companion address can bind only one instance.

The local-radio implementation exchanges signed adverts, end-to-end encrypted
direct messages, acknowledgements, and shared-key group messages. It can also
carry a peer-bound SolarOS virtual serial port through ordinary encrypted
MeshCore direct packets. SolarOS does not repeat traffic or include MeshCore's
Arduino interface, room server, remote administration, sensors, or telemetry.
SolarOS Link remains a separate protocol; the virtual serial feature only
reuses its reliable stream framing.

## Quick start

Attach a packet-capable LoRa radio and start the worker with an explicit
regional profile:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
job start meshcore radio0 meshcore-eu868
meshcore status
chat
```

`meshcore-us915` is the MeshCore "USA/Canada (Recommended)" preset: 910.525 MHz,
62.5 kHz, SF7, coding rate 4/5. `meshcore-eu868` is 869.618 MHz, 62.5 kHz, SF8, coding rate 4/8, a 32-symbol
preamble, sync word `0x12`, CRC, variable packet length, and 14 dBm. It is an
EU868 profile; do not use it outside regions where that frequency and transmit
behavior are legal. The profile argument is mandatory so SolarOS never silently
chooses a region.

The job claims the radio, applies the complete profile, enters receive mode,
and emits one zero-hop advert. It restores the previous radio configuration and
state and releases ownership when stopped or when startup fails. Starting
`radio-link` on the same radio fails with the normal ownership error.

```text
job status meshcore
job stop meshcore
```

## BLE companion transport

Use the peer address and address type reported by `ble scan`. Supply the
companion's six-digit PIN to request BLE encryption and bonding:

```text
job start meshcore-ble 68:ee:8f:69:5f:35 public 123456
job status meshcore-ble
contacts
chat
```

The job discovers the MeshCore UART-style GATT service, subscribes to device
notifications, starts protocol version 3, sets the device time, and synchronizes
contacts, all eight channel slots, and queued messages. New adverts trigger a
contact refresh, and a messages-waiting notification drains the device queue.
Direct sends remain in `sending` state until the companion reports the expected
MeshCore acknowledgement; group sends become `sent` after the device accepts
them.

Job status lists each instance, its connection state, and its current detail.
SolarOS can retain up to 512
contacts across providers; imports do not evict existing contacts when this
store is full. The companion's own firmware can impose a smaller limit.
The status bar shows the radio icon while the companion is online. Group
labels identify `[companion]` or `[radio]`; groups absent from the current
channel configuration remain readable as `(history)` entries.

The optional PIN is saved in the instance profile on mounted storage. Status
and enumeration do not expose it; setup commands are excluded from shell and
Chat input history. MeshCore's direct-message and shared-channel
protection remains independent of BLE transport encryption. The job reconnects
with bounded exponential backoff after a disconnect. SolarOS's multi-peer BLE
host lets an established BLE keyboard remain connected while the companion is
online.

The external device remains authoritative for its identity, channels, radio
region, and contact routing. Configure those on the companion device. The
`meshcore` shell command applies only to the local-radio job, and virtual serial
ports are currently available only with that local implementation.

## Named instances

Create independent connections with the common Messaging commands:

```text
messages add meshcore field radio radio0 meshcore-eu868
messages add meshcore desk ble 68:ee:8f:69:5f:35 public 123456
messages connect field
messages connect desk
job start meshcore
job start meshcore-ble
messages join field #general
messages instances
```

The jobs take no arguments when using saved bindings. Legacy starts with radio
or companion arguments save and enable that binding, using the default instance
when it is unbound or allocating a new instance otherwise. Connection failures
retry with bounded backoff. Resource admission still depends on available radios,
BLE peer slots, shared credential capacity, and memory.

`messages join NAME CHANNEL` selects a configured group. On a local radio,
joining a new `#hashtag` creates its derived channel key; joining `Public` enables
the built-in public channel. BLE groups must already exist on the companion.
`messages query NAME PUBLIC_KEY_HEX` opens a direct conversation through that
instance. Local channel, name, and identity settings also use the common interface:

```text
messages configure field name field-unit
messages configure field channel team BASE64_PSK
messages configure field public off
messages configure field identity generate
```

These changes stop the affected local runtime while applying settings; its job
restarts it when enabled. Other instances continue running. `messages configure
NAME radio RADIO PROFILE` or `messages configure NAME ble ADDRESS TYPE [PIN]`
changes a binding within its backend type. Chat accepts the same settings after
`/add` and `/configure`. Legacy `meshcore` commands address the default instance
when it is unbound or uses a local radio. If that default is absent or uses BLE,
they use the sole local-radio instance. With several local-radio instances and
no local default, use `messages instances` to list them and the named Messaging
commands to manage each one.

`messages remove NAME` or `/remove NAME` closes that instance, cancels its sends,
and deletes its profile credentials and local message history. Default instances
can also be removed. Settings use `.chat/profiles`; identity and channel keys use
`.settings/credentials.bin`. The previous NVS name and public-channel keys migrate
only after a verified profile save. BLE bonding keys remain owned by the BLE stack.
Remove any virtual serial ports bound to a local instance before removing it;
an instance with those resources still attached reports an invalid state.

## Identity, name, and adverts

The first start generates an Ed25519 identity from the SolarOS RNG and stores
it as an opaque Credentials record. A MeshCore-specific advertised-name
override is optional; without one, SolarOS uses the truncated device
`user@hostname` identity.

```text
meshcore identity show
meshcore identity generate
meshcore identity generate --force
meshcore identity import PRIVATE_KEY_HEX
meshcore identity export --private
meshcore name
meshcore name field-unit
meshcore advert zero
meshcore advert flood
```

Generating with `--force`, importing, or editing channels is rejected while the
job runs. Private export is deliberately explicit and prints a warning.
Treat the output as a password. Identity and group keys use the unencrypted
mounted credential file; access to its volume can recover those secrets.

No periodic advert is sent. `meshcore advert zero` reaches only local receivers;
`meshcore advert flood` deliberately requests a network-wide flood.

## Contacts, trust, and messages

A valid advert proves that its sender controls the advertised public key. It
does not prove the person's identity, so the endpoint enters Contacts as
`discovered`. Review it with `contacts show`, then use `contacts trust` or
`contacts block`. Blocked endpoints cannot send or receive direct messages.

Direct messages use MeshCore's public-key/ECDH path. An outbound message waits
for its cryptographic acknowledgement and retries twice after the initial
attempt. A valid ACK marks it delivered; the final timeout marks it failed.
Discovered recipients require Chat confirmation or `messages send
... --allow-untrusted`.

The standard `Public` group is enabled by default. Group messages are encrypted
with a shared key but their embedded sender name is not authenticated, so Chat
always labels them sender-unverified. A group transmission becomes sent after
the radio finishes because groups have no recipient ACK.

```text
meshcore channel list
meshcore channel public off
meshcore channel public on
meshcore channel add '#hansemesh'
meshcore channel add team BASE64_PSK
meshcore channel remove team
```

There are eight total group slots including Public. Custom keys must decode to
16 or 32 bytes and remain confined to Credentials; they are not shown by
Inbox, logs, autocomplete, agent tools, Python, or Lua.

A channel name that starts with `#` is a public hashtag channel. SolarOS derives
its 16-byte key from the first 16 bytes of the SHA-256 digest of the exact
channel name, including the leading `#`. Hashtag names are case-sensitive and
do not take a pre-shared-key argument. Anyone who knows or guesses the name can
derive the same key, so hashtag channels are not private. Use the explicit
Base64 key form only for private channels without the `#` prefix.

## Reliable virtual serial ports

A MeshCore stream is a normal SolarOS byte-stream port carried in encrypted
MeshCore direct-request packets. Standard MeshCore repeaters can route those
opaque packets between two SolarOS endpoints; the repeaters do not need
SolarOS support. Create the stream explicitly on both devices and bind each
side to the other device's trusted MeshCore endpoint.

First start MeshCore on both devices. Use flood adverts when the endpoints are
not in direct radio range, then review and trust the discovered endpoint on
each side:

```text
job start meshcore radio0 meshcore-eu868
meshcore advert flood
contacts list
contacts show <contact-id>
contacts trust <contact-id> <endpoint-id>
meshcore stream create mser0 <endpoint-id>
meshcore stream status mser0
```

Endpoint IDs are local database IDs and can differ between the two devices.
The endpoint must be a trusted MeshCore endpoint with the exact peer public
key. A discovered, blocked, group, or changed identity is rejected. Configure
only one stream for a peer identity.

For example, expose a shell on the internet-connected ground station:

```text
session create shell mser0 --term dumb
```

On the pocket terminal, open it as a serial connection:

```text
com mser0
```

To expose another machine instead, connect its serial interface to the ground
station and bridge the UART port:

```text
job start bridge uart0 mser0
```

The stream provides ordered serial delivery with up to two frames in flight and
receiver credits. It carries up to 78 data bytes per MeshCore packet and uses
2048-byte transmit and receive queues. Acknowledgements piggyback on replies;
standalone ACKs become due after 250 milliseconds, or earlier after two new
frames. Retries take 12 to 16 seconds, and a peer is
treated as absent after 180 seconds. Both endpoints must support Link stream
protocol v3. Interactive command shells are practical; full-screen TUIs,
bulk transfers, and chatty terminal negotiation are poor fits. Use the `dumb`
terminal mode where possible.

At most two Link-backed streams can exist at once, including MeshCore streams.
Stream definitions remain in RAM if the MeshCore job stops and reconnect after
it starts again, but they do not persist across a reboot. Remove an unclaimed
port with `meshcore stream remove <port>`.

## Requirements and diagnostics

Both transports require PSRAM. Local-radio messaging also needs a supported
packet radio; the companion transport needs BLE. Use `meshcore status` for
traffic, packet usage, retries, and duplicates. If startup reports no memory,
check `mem` and stop unused jobs: the local-radio worker needs a 7168-byte
internal stack and the BLE companion needs 8192 bytes, plus other buffers.

## Quick reference

```text
meshcore status
meshcore identity show
meshcore identity generate [--force]
meshcore identity import <private-key-hex>
meshcore identity export --private
meshcore name [name]
meshcore advert zero|flood
meshcore channel list
meshcore channel add <#hashtag>
meshcore channel add <name> <base64-psk>
meshcore channel remove <name>
meshcore channel public on|off
meshcore stream list
meshcore stream status [port]
meshcore stream create <port> <trusted-endpoint-id>
meshcore stream remove <port>
job start meshcore <radio> <profile>
job status meshcore
job stop meshcore
job start meshcore-ble <address> <public|random|rpa_public|rpa_random> [six-digit-pin]
job status meshcore-ble
job stop meshcore-ble
```
