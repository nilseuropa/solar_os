+++
id = "meshcore"
title = "MeshCore companion messaging"
section = "service"
summary = "Secure direct and shared-group messages over a claimed packet radio"
aliases = ["meshcore.job", "meshcore.radio"]
keywords = "meshcore lora radio identity advert contacts direct group public psk trust"
packages_any = ["service_meshcore", "job_meshcore"]
+++
# MeshCore companion messaging

SolarOS implements a non-forwarding MeshCore companion node. It exchanges
signed adverts, end-to-end encrypted direct messages, acknowledgements, and
shared-key group messages through the provider-neutral Contacts and Messages
services. It does not repeat traffic or include MeshCore's Arduino interface,
companion protocol, room server, remote administration, sensors, or telemetry.
SolarOS Link remains a separate protocol.

## Quick start

Attach a packet-capable LoRa radio and start the worker with an explicit
regional profile:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
job start meshcore radio0 meshcore-eu868
meshcore status
chat
```

`meshcore-eu868` is 869.618 MHz, 62.5 kHz, SF8, coding rate 4/8, a 32-symbol
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
Treat the output as a password: current NVS is not encrypted, and physical
flash access can recover the stored identity and group keys.

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
meshcore channel add team BASE64_PSK
meshcore channel remove team
```

There are eight total group slots including Public. Custom keys must decode to
16 or 32 bytes and remain confined to Credentials; they are not shown by
Inbox, logs, autocomplete, agent tools, Python, or Lua.

## Implementation and limits

The complete provider context and its 16-packet pool use external-required
PSRAM. Runtime measurement tuned the worker to a 7168-byte internal stack.
`meshcore status`
reports packet usage, traffic, retries, duplicate counts, whether the context
is in PSRAM, and the measured minimum stack watermark. Firmware packages omit
MeshCore unless the board has PSRAM and packet-radio expansion capability.

SolarOS vendors the audited protocol subset from MeshCore commit
`03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a`. Updates are explicit source
reviews; firmware builds never download protocol code.

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
meshcore channel add <name> <base64-psk>
meshcore channel remove <name>
meshcore channel public on|off
job start meshcore <radio> <profile>
job status meshcore
job stop meshcore
```
