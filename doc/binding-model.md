# Stream, Control, Parameter, MIDI, and OSC Binding Model

SolarOS uses four core concepts for values that can be measured, mapped, or
controlled: streams, controls, parameters, and bindings. MIDI and OSC connect
to that model through bounded transports. Together they form a constrained
directed graph, not one generic event bus or an arbitrary patch bay.

## Overview

```text
                                 SolarOS

  VALUE SOURCES                  POLICY / STATE                 TARGETS

  MIDI IN -- configured CC --> midi.cc.<channel>.<cc>
                               native scalar 0..127
                                         |
                                         v

  +----------------+             +------------------+           +-------------+
  | scalar stream  |-- sample -->| named control    |-- bind -->| app         |
  | native float   | controls job| 0..65535         | parameter | parameter   |
  | adc1, battery, |             |                  |           | native unit |
  | temperature...|             | calibrate        |           +-------------+
  +----------------+             | smooth           |
          |                      | deadband         |-- bind --> MIDI CC
          |                      | invert           |           0..127
          |                      +------------------+
          |                               |
          | OSC stream binding            | OSC control binding
          | native float32                 | normalized float32 0.0..1.0
          v                               v
  +--------------------------------------------------------------------------+
  | job.osc: named outbound bindings -> user-selected OSC addresses          |
  +--------------------------------------------------------------------------+
                                      |
                                      v
                                  OSC peer

  +----------------+                  ^
  | event stream   |-- OSC binding ---|
  | sampled bool   |   edge filter, OSC int32 0 or 1
  +----------------+

  OSC peer
      |
      | exact /solaros/parameter/<owner>/<name>[/normalized] address
      | one native value, or normalized float32 0.0..1.0
      v
  +----------------+       +----------------------+       +------------------+
  | job.osc UDP    |------>| parameter registry   |------>| application      |
  | input          |       | range + step checks  |       | setter callback  |
  +----------------+       +----------------------+       +------------------+
```

The two OSC directions are deliberately different:

- Incoming OSC addresses map automatically to live parameter paths.
- Outgoing OSC messages require explicit named bindings to streams or controls.
- Parameter changes are not echoed automatically. There is no parameter-to-OSC
  binding in this version.

## What each object owns

| Object | Owns | Value domain | Lifetime |
| --- | --- | --- | --- |
| Scalar stream | A readable measurement or signal source | Native float and unit | Registered by its provider |
| MIDI CC stream | The latest value for one configured incoming channel/controller pair | Native `0..127` | Volatile definition; readable after MIDI receives a value |
| Event stream | A readable boolean/event source | Currently one sampled byte for GPIO | Registered by its provider |
| Named control | Calibration, smoothing, deadband, inversion, normalized state | `0..65535` internally | Volatile runtime configuration |
| Parameter | App callback, native range, unit, step, and curve | App-defined native value | Present while the owning app registers it |
| Control binding | One named control to one parameter or MIDI CC target | Normalized conversion | Volatile runtime configuration |
| OSC binding | One stream or control source to one OSC address | OSC float32 or int32 | Volatile runtime configuration |
| OSC parameter mapping | One exact incoming OSC namespace to a live parameter | Native value, or normalized `0.0..1.0` with the final `/normalized` suffix | Automatic; no stored binding |

Numeric binding IDs are internal bookkeeping. Shell commands use semantic names
such as `cutoff`, `ambient`, and `button`.

## Supported routes

```text
scalar stream -> named control -> app parameter
scalar stream -> named control -> MIDI CC
incoming MIDI CC -> configured scalar stream -> named control -> app parameter
scalar stream -> named control -> outbound OSC float32 0.0..1.0
scalar stream -----------------> outbound OSC float32 in native units
event stream ------------------> outbound OSC int32 0 or 1 on selected edges
manual/script value -> named control -> the same control targets
incoming OSC native value -----> live app parameter in native units
incoming OSC normalized value -> parameter curve -> live app parameter
```

The following routes do not exist in this version:

```text
parameter -> automatic OSC echo
incoming OSC -> named control
event stream -> named continuous control
foreground keyboard/input queue -> OSC
arbitrary OSC wildcard -> arbitrary service callback
```

These omissions keep controls independent of networking, prevent OSC from
stealing foreground input events, and avoid implicit feedback loops.

## Value conversion

```text
scalar native value
      |
      | input_min..input_max, smoothing, deadband, optional inversion
      v
named control 0..65535
      |
      +--> parameter binding --> declared linear/log curve --> native parameter
      |
      +--> MIDI binding -------------------------------> 0..127
      |
      +--> OSC control binding ------------------------> 0.0..1.0 float32

scalar stream OSC binding -----------------------------> native float32
event stream OSC binding ------------------------------> int32 0 or 1
incoming OSC native parameter message -----------------> native parameter value
incoming OSC normalized message --> declared curve ----> native parameter value

incoming MIDI CC 0..127 -> configured scalar stream -> named control 0..65535
                            -> declared curve ---------> native parameter value
```

An incoming OSC value for `/solaros/parameter/synth/filter/cutoff` is expressed
in Hz. Appending `/normalized` selects `0.0..1.0` instead. The normalized route
accepts float32, `True`, and `False`, but rejects int32 to keep its meaning
unambiguous. In both cases the parameter registry remains authoritative for the
declared range and step; normalized values also use its logarithmic or linear
curve.

## Example: one knob, local Synth, and remote OSC

```text
adc1 (mV)
   |
   v
control "cutoff" (0..65535)
   |                              |
   | control binding              | OSC binding
   v                              v
synth.filter.cutoff          /surface/cutoff
(Hz, with pickup)            (float32 0.0..1.0)
```

```sh
control create cutoff adc1 100 3200 smooth=40 deadband=8
control bind cutoff parameter synth.filter.cutoff pickup=on
osc bind cutoff-out control cutoff /surface/cutoff rate=50

job start controls
job start osc listen=9000 target=192.168.1.50:9001
synth
```

The parameter binding can exist while Synth is closed. It starts applying when
Synth registers `synth.filter.cutoff`. `pickup=on` prevents the physical knob
from causing a jump when a preset, the UI, or incoming OSC changes the current
parameter. If the target moves far enough after pickup, soft takeover rearms.

## Example: direct telemetry without a control

Use a direct stream binding when the remote peer needs the physical value and
SolarOS does not need calibration or a normalized control:

```sh
osc bind ambient stream temperature /room/temperature rate=2 delta=0.1
osc bind voltage stream battery /device/battery rate=1 delta=0.01
job start osc listen=9000 target=192.168.1.50:9001
```

The OSC values retain the native stream unit. A temporarily missing OSC source
remains configured and is retried by the OSC job.

## Example: external OSC controls a live parameter

```text
OSC /solaros/parameter/synth/filter/cutoff ,f 1200.0
  -> synth.filter.cutoff
  -> range and step validation
  -> Synth setter receives 1200 Hz
```

There is no stored incoming binding. The address is derived from the parameter
path. If Synth is closed, the path is unknown and the job records that result.

The normalized form uses the same parameter without requiring the sender to
know its native range or curve:

```text
OSC /solaros/parameter/synth/filter/cutoff/normalized ,f 0.5
  -> synth.filter.cutoff normalized midpoint
  -> logarithmic curve and step quantization
  -> Synth setter receives approximately 632 Hz
```

## Example: one SolarOS device controls another

```text
controller device                                      synth device

adc1 -> control "cutoff" -> OSC 0.0..1.0   UDP   /normalized -> cutoff parameter
```

On the controller device:

```sh
control create cutoff adc1 100 3200 smooth=20 deadband=8
osc bind cutoff-out control cutoff /solaros/parameter/synth/filter/cutoff/normalized
job start controls
job start osc target=192.168.1.40:9000
```

On the synth device:

```sh
job start osc listen=9000 peer=192.168.1.30
synth
```

The ADC calibration stays on the controller. The cutoff range, curve, and step
stay on the synth device.

## Example: incoming MIDI CC controls Synth cutoff

```text
MIDI channel 1 CC 74 -> midi.cc.1.74 -> control "cutoff"
  -> synth.filter.cutoff normalized mapping
  -> logarithmic cutoff curve and native step
```

```sh
midi stream add 1 74
control create cutoff midi.cc.1.74 0 127
control bind cutoff parameter synth.filter.cutoff pickup=off
job start midi midi0
job start controls
synth
```

The MIDI CC stream is an explicit, non-consuming view of the latest matching
value. Synth and other MIDI subscribers still receive the original message. Up
to 16 CC streams can be configured without reserving entries for every possible
channel/controller pair.

## Example: sampled GPIO event

```sh
osc bind button stream gpio17 /surface/button edge=both rate=50
```

Current `gpioN` event streams expose sampled state, not a queued interrupt-edge
history. The OSC binding detects changes between samples and can miss a pulse
that rises and falls inside one sampling interval. True edge delivery needs a
timestamped fan-out event provider.

## Scheduling and ownership

- `job start controls` samples configured scalar streams at 50 Hz and applies
  control-to-parameter or control-to-MIDI bindings.
- `midi stream add` registers a volatile scalar stream for one incoming MIDI
  channel/controller pair. It waits for a value whenever the MIDI job starts.
- A manual control can be changed with `control set` without a source stream.
- `job start osc` owns UDP input and independently samples outbound OSC sources
  at each binding's `rate=`.
- `service.controls` does not depend on OSC or networking. `service.osc`
  consumes controls and streams without reversing that dependency.
- Control and OSC definitions are volatile. Restore them from
  `/.shell/startup` when needed after reboot.
- Creating a stream-backed control requires that scalar stream to exist at
  creation time. Parameter bindings can wait for an app parameter to appear;
  OSC bindings can wait for a missing stream or control source to appear.

## Feedback and arbitration

SolarOS does not provide multi-writer arbitration for a parameter. The app UI,
a control binding, and incoming OSC can all call the same parameter setter.
The latest accepted write wins.

Automatic OSC echo is intentionally absent, so an incoming message does not by
itself create a network loop. If an external peer manually echoes outbound
control values back to a parameter, that behavior belongs to the peer's patch.
Use `pickup=on` for physical controls when another writer can move the target.

## Inspection

```sh
stream
midi stream list
control list
control bindings
control parameters
osc bindings
job status controls
job status midi
job status osc
```

These commands show registered providers, MIDI CC sources, normalized control
state, live parameter targets, and OSC transport/binding state.
