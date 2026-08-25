+++
id = "osc"
title = "Open Sound Control"
section = "network"
summary = "Control live app parameters and publish named stream or control bindings over OSC 1.0 UDP"
aliases = ["open-sound-control"]
keywords = "osc open sound control udp synth funcgen parameter stream event control binding telemetry"
packages_any = ["service_osc", "job_osc"]
+++
# Open Sound Control

SolarOS implements a bounded OSC 1.0 subset over IPv4 UDP. It accepts exact
addresses for live native application parameters and sends explicitly named
stream, event-stream, or normalized-control bindings to one configured target.

OSC has no authentication or encryption. Keep the job stopped until it is
needed. Use it only on a trusted LAN or SoftAP, or carry the traffic through a
trusted WireGuard tunnel. The optional `peer=` setting restricts incoming
packets to one IPv4 address; it is an allowlist, not authentication.

## Incoming parameters

Start the job with a UDP listening port:

```text
job start osc listen=9000
```

The default port is `9000`. Incoming addresses map automatically to parameters
registered by the currently running application:

```text
/solaros/parameter/synth/filter/cutoff
/solaros/parameter/synth/osc2/mix
/solaros/parameter/funcgen/frequency
```

Each message must contain exactly one OSC float32, int32, `True`, or `False`
argument. SolarOS converts the address after `/solaros/parameter/` from slash
components to the native dotted path and calls the shared parameter setter.
The parameter service remains authoritative for range checks and step
quantization. Synth and Funcgen publish their parameters only while they are
running, so a write to a closed application increments the `unknown` counter.

Immediate OSC bundles are accepted with at most two bundle levels and eight
messages per packet. Future timetags, address patterns, blobs, MIDI values,
automatic echo, OSCQuery, and scheduled execution are not part of this version.
Packets are limited to 512 bytes and accepted traffic is limited to 100 packets
per second.

## Outgoing bindings

Set one destination when the job starts:

```text
job start osc listen=9000 target=192.168.1.50:9001
```

Bindings have stable semantic names and user-selected OSC addresses:

```text
osc bind ambient stream temperature /room/temperature rate=2 delta=0.1
osc bind voltage stream battery /device/battery rate=1 delta=0.01
osc bind cutoff-out control cutoff /surface/cutoff
osc bind button stream gpio17 /surface/button edge=both
```

Scalar streams send a float32 in the stream's native unit. Named controls send
a normalized float32 in the range `0.0..1.0`; calibration, smoothing, deadband,
and inversion remain owned by `service.controls`. Event streams send int32 `1`
on a rising edge and int32 `0` on a falling edge, filtered by
`edge=rising|falling|both`.

`rate=` accepts `0.1..100` Hz and defaults to 50 Hz. Scalar and control
bindings are change-only by default. `delta=` sets the minimum change in the
source's native value, while `send=always` sends the current value at each rate
interval. A missing source remains configured and is retried automatically.

Current GPIO event streams expose sampled state. They are not interrupt-edge
queues, so OSC can miss a pulse that starts and ends between samples. OSC does
not read the foreground input queue and cannot steal keyboard events from the
active application.

Inspect or remove runtime bindings with:

```text
osc bindings
osc unbind ambient
osc clear
```

Bindings are volatile. Put the `osc bind` and `job start osc` commands in
`/.shell/startup` when they must be restored after reboot.

## Status and filtering

```text
job start osc listen=9000 target=192.168.1.50:9001 peer=192.168.1.50
job status osc
job stop osc
```

Detailed job status reports the listening port, target, peer filter, inbound
packet/application/error counters, outbound sends and failures, source errors,
and binding count. `osc bindings` adds each source state, last value, last send
time, and last error.

## Quick reference

```text
job start osc [listen=port] [target=host:port] [peer=ipv4]
job status osc
job stop osc
osc bindings
osc bind <name> stream <stream> <address> [rate=hz] [delta=value] [send=change|always]
osc bind <name> stream <event-stream> <address> edge=rising|falling|both [rate=hz]
osc bind <name> control <control> <address> [rate=hz] [send=change|always]
osc unbind <name>
osc clear
```
