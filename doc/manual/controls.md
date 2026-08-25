+++
id = "controls"
title = "Continuous controls"
section = "hardware"
summary = "Map analog and other scalar inputs to app parameters or MIDI CC"
aliases = ["control", "knobs", "potentiometers"]
keywords = "control controls adc analog potentiometer knob mapping parameter midi cc pickup smoothing deadband synth cutoff"
packages_any = ["service_controls"]
+++
# Continuous controls

SolarOS controls turn scalar streams into named normalized values. A control
can drive one or more native foreground-app parameters or MIDI Control Change
messages. This keeps ADC calibration, smoothing, deadband, inversion, and
pickup behavior independent of the destination.

List the scalar streams that exist on the running board:

```text
stream
```

For a potentiometer whose wiper is connected to an ADC-capable expansion pin,
create a control using its measured millivolt endpoints. This example uses
`adc1` and maps 0 through 3300 mV to the full control range:

```text
control create cutoff adc1 0 3300 smooth=40 deadband=8
```

`smooth=` is an exponential smoothing time in milliseconds. `deadband=` uses
the source unit, so it is millivolts for an ADC stream. Add `invert` when the
physical direction is reversed. Use the actual endpoint readings when the
potentiometer does not reach 0 or 3300 mV.

## Native application parameters

A running native application can publish tunable parameters with stable paths,
ranges, units, steps, and linear or logarithmic curves. Inspect the parameters
that are currently available:

```text
control parameters
```

The Synth app publishes parameters such as `synth.filter.cutoff`. Bind the
potentiometer to it and start the sampler:

```text
control bind cutoff parameter synth.filter.cutoff pickup=on
job start controls
synth
```

Pickup is soft takeover. The hardware knob does not change the parameter until
it reaches or crosses the app's current value, preventing a sudden jump when a
preset loads or the app resumes. The binding remains configured while its app
is suspended or stopped and applies again when the parameter path returns.

For direct inspection or testing:

```text
control parameter get synth.filter.cutoff
control parameter set synth.filter.cutoff 1200
```

## MIDI CC

### Control to MIDI CC

A control can drive a MIDI controller after the MIDI bus and worker are
running:

```text
expansion bus create midi midi0 tx=gpio2 rx=gpio3
job start midi midi0
control bind cutoff midi 1 74
job start controls
```

The normalized 16-bit control value is scaled to the MIDI range `0..127`.
Channel numbers are `1..16`; controller numbers are `0..127`.

### MIDI CC to application parameter

Expose an incoming MIDI controller as a scalar stream, then use the same
control-to-parameter path as an ADC or other measurement:

```text
expansion bus create midi midi0 tx=gpio2 rx=gpio3
midi stream add 1 74
control create cutoff midi.cc.1.74 0 127
control bind cutoff parameter synth.filter.cutoff pickup=off
job start midi midi0
job start controls
synth
```

`midi stream add <channel> <controller>` registers an exact scalar stream named
`midi.cc.<channel>.<controller>`. It reports `0..127` and retains the latest
matching value while the MIDI job is running. It is non-consuming, so MIDI
subscribers such as Synth still receive the original message.

Up to 16 MIDI CC streams can be configured. Explicit registration avoids
reserving stream-registry entries for all 2,048 possible channel/controller
pairs. A new stream reports `waiting` until its first matching message. It
returns to that state whenever the MIDI job stops or restarts.

Inspect or remove the definitions with:

```text
midi stream list
midi stream remove 1 74
midi stream clear
```

MIDI CC stream definitions are volatile. Restore `midi stream add` commands
from `/.shell/startup` with the related control and job commands when needed.

## Manual and script controls

Use `manual` instead of a stream when a Python or Lua program supplies the
value:

```text
control create expression manual 0 1
control set expression 32768
```

Python and Lua expose values as floating-point numbers from `0.0` through
`1.0`:

```python
import solaros

solaros.controls.set("expression", 0.5)
print(solaros.controls.get("expression"))
```

```lua
local solaros = require("solaros")

solaros.controls.set("expression", 0.5)
print(solaros.controls.get("expression"))
```

## Inspection and removal

```text
control list
control bindings
control get cutoff
control unbind cutoff
control delete cutoff
control clear
job stop controls
```

`control list` reports normalized and raw values, sample and update counts, and
the last source error. `control bindings` reports target state, pickup state,
application counts, and the last target error. `control unbind <name>` removes
all parameter and MIDI targets owned by that control. Control and binding
definitions are runtime configuration; place the creation, binding, and
job-start commands in `/.shell/startup` to restore them after reboot.

## Quick reference

Controls normalize scalar streams to `0..65535`, apply optional smoothing,
deadband, and inversion, and fan out to typed targets. Native app targets can
use soft takeover and survive temporary parameter absence. MIDI targets emit
CC values from `0..127`. Python and Lua use normalized values from `0.0` to
`1.0`.
