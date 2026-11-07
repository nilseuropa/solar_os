+++
id = "osc"
title = "Open Sound Control"
section = "network"
summary = "Receive scalar stream commands, control live parameters and publish named OSC bindings over UDP"
aliases = ["open-sound-control"]
keywords = "osc open sound control udp synth funcgen parameter stream event control binding telemetry"
packages_any = ["service_osc", "job_osc"]
+++
# Open Sound Control

SolarOS implements a bounded OSC 1.0 subset over IPv4 UDP. It accepts exact
addresses for live native application parameters and explicitly bound scalar
stream sinks. It sends named stream, event-stream, or normalized-control
bindings to one configured target.

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
registered by the currently running application. Use the parameter address for
native units, or append `/normalized` for a normalized value:

```text
/solaros/parameter/synth/filter/cutoff
/solaros/parameter/synth/filter/cutoff/normalized
/solaros/parameter/synth/osc2/mix
/solaros/parameter/funcgen/frequency
```

The native address accepts exactly one OSC float32, int32, `True`, or `False`
argument in the parameter's native unit. The `/normalized` address accepts one
float32 in the range `0.0..1.0`, or `True` and `False` for the endpoints. It
rejects int32 arguments so integer native values cannot be mistaken for
normalized values.

SolarOS converts the address after `/solaros/parameter/` from slash components
to the native dotted path. The parameter service applies the declared range,
linear or logarithmic curve, and step quantization. Synth and Funcgen publish
their parameters only while they are running, so a write to a closed
application increments the `unknown` counter.

Immediate OSC bundles are accepted with at most two bundle levels and eight
messages or mapped updates per packet. Future timetags, address patterns, blobs, MIDI values,
automatic echo, OSCQuery, and scheduled execution are not part of this version.
Packets are limited to 512 bytes and accepted traffic is limited to 100 packets
per second.

## Incoming stream bindings

Map an exact OSC address and a zero-based argument index to a writable float32
scalar stream:

```text
osc bind base-linear receive diff_drive.base.cmd_linear /base/cmd_vel arg=0
osc bind base-angular receive diff_drive.base.cmd_angular /base/cmd_vel arg=1
job start osc listen=9000 peer=192.168.1.50
```

The example maps a message with type tags `,ff` and arguments `0.02, 0.0` to
the drive's linear and angular command streams. Start the robot to make its
streams available. The same binding syntax works for standalone controller
setpoints, servo commands and other scalar sinks. `arg=` defaults to zero and
accepts 0..7. Messages accept up to eight float32, int32 or Boolean arguments.
Values retain the target stream's native unit, without normalization or scaling.

A desktop Python sender can send both values in one UDP packet:

```python
import socket
import struct
import time

def osc_string(text):
    data = text.encode("ascii") + b"\0"
    return data + b"\0" * (-len(data) % 4)

packet = (osc_string("/base/cmd_vel") + osc_string(",ff")
          + struct.pack(">ff", 0.02, 0.0))
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
    for _ in range(40):
        sender.sendto(packet, ("192.168.1.40", 9000))
        time.sleep(0.05)
```

This publishes at about 20 Hz for two seconds. Each received packet updates
the mapped streams once. The receiver never repeats a cached command between
packets, so stopping the sender allows the target's command watchdog to expire.
Commands use local receipt/write time; remote-clock synchronization and
out-of-order packet detection are not provided.

All mapped arguments and target types, directions and declared ranges are
checked before any stream in that message is written. Writers are acquired
together on the first valid message and held until `job stop osc`, unbind or
clear. Configuration alone does not claim outputs. A missing stream is retried
on the next packet. A target already owned by another writer is rejected.
Only one receive binding may target a given stream.

Writes within a message or bundle are sequential, not an atomic transaction.
A provider failure can leave a partial update. Malformed packets are rejected
before any update; immediate bundles share the eight-update limit. Extra
numeric arguments are allowed, but every mapped argument must be present.
The `/solaros/parameter/` namespace remains reserved for parameter writes.

Stopping OSC releases writers without sending an extra value; the target's
hold/stop/watchdog policy applies. Stop OSC before stopping a robot whose
streams it holds. Failed closes retain the binding and handle for retry with
`osc unbind` or `osc clear`.

## Sending from the shell

Send one OSC message directly, without bindings or a running OSC job:

```text
osc send 192.168.1.238:9000 /base/cmd_vel 0.02 0
```

The destination is an IPv4 address or hostname followed by a UDP port from 1
to 65535. One to eight finite numeric values are encoded as float32 arguments
in a single message, in the order supplied. The example sends type tags `,ff`;
the receive bindings above map its values to linear and angular velocity.

Use the same repeat options as `stream write` when the receiver needs timely
updates:

```text
osc send 192.168.1.238:9000 /base/cmd_vel 0.02 0 --rate 20 --duration 2
```

`--rate` accepts integer rates from 1 to 100 Hz. Without `--duration`, publishing
continues until Ctrl+C, Esc, app exit, or disconnection. `--duration` accepts
0.001..86400 seconds and requires `--rate`. Missed periods are skipped rather
than followed by a catch-up burst. The socket closes on completion, cancellation
or failure. No extra zero command is sent on exit; the receiver's watchdog or
hold policy applies. Sending does not change the OSC job or its bindings.
Tab completes `send` and the unused repeat options after the values.

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

## Python and Lua

Both runtimes expose the same native OSC service as `solaros.osc`:

```python
solaros.osc.bind_stream(
    "ambient", "temperature", "/room/temperature", 2.0, 0.1
)
solaros.jobs.start(
    "osc", ["listen=9000", "target=192.168.1.50:9001"]
)
```

Lua uses the same names and positional arguments. `bind_stream`, `bind_event`,
`bind_control` and `bind_receive` return binding IDs. `bindings()` returns the complete
runtime state; `unbind()` and `clear()` remove definitions. The optional rate
is in Hz, scalar-stream delta uses native units, and the final Boolean selects
send-always behavior for scalar and control bindings.

`bind_receive(name, stream, address[, argument])` creates an incoming binding;
the argument index is zero-based in both runtimes. Receive records include
`direction="receive"`, `source` (the local target stream), `argument`,
`target_available`, `target_claimed`, `received`, `write_errors`,
`last_receive_ms` and `last_error`. `clear()` reports cleanup errors rather
than discarding a writer that could not be closed.

`encode_float(address, value)` and `encode_int(address, value)` return binary
messages for managed UDP APIs. `dispatch(packet)` applies the same bounded
parameter/stream parser used by the OSC job and reports message, applied, unknown, and
rejected counts. `limits()` reports the public packet, address, binding,
bundle, update, and rate bounds. Socket ownership and filtering remain with the
native job and `solaros.jobs` API.

## SolarOS controller and synth devices

A normalized address lets one SolarOS device use a calibrated physical control
to drive a parameter on another device without duplicating its native range or
curve. On the controller device:

```text
control create cutoff adc1 100 3200 smooth=20 deadband=8
osc bind cutoff-out control cutoff /solaros/parameter/synth/filter/cutoff/normalized
job start controls
job start osc target=192.168.1.40:9000
```

On the synth device:

```text
job start osc listen=9000 peer=192.168.1.30
synth
```

The controller sends `0.0..1.0`. The synth device converts it through the live
parameter's logarithmic cutoff curve and native range.

## Status and filtering

```text
job start osc listen=9000 target=192.168.1.50:9001 peer=192.168.1.50
job status osc
job stop osc
```

Detailed job status reports the listening port, target, peer filter, inbound
packet/application/error counters, outbound sends and failures, source errors,
and binding count. `osc bindings` shows outgoing source state and send counters,
or incoming argument mappings, target claims, receive/write-error counters,
last receive time and last error.

## Quick reference

```text
job start osc [listen=port] [target=host:port] [peer=ipv4]
job status osc
job stop osc
osc bindings
osc send <host:port> <address> <value> [value ...] [--rate 1..100] [--duration seconds]
osc bind <name> stream <stream> <address> [rate=hz] [delta=value] [send=change|always]
osc bind <name> stream <event-stream> <address> edge=rising|falling|both [rate=hz]
osc bind <name> control <control> <address> [rate=hz] [send=change|always]
osc bind <name> receive <scalar-sink> <address> [arg=0..7]
osc unbind <name>
osc clear
```

Python/Lua: `solaros.osc.bindings`, `bind_stream`, `bind_event`,
`bind_control`, `bind_receive`, `unbind`, `clear`, `encode_float`, `encode_int`, `dispatch`,
and `limits`.
