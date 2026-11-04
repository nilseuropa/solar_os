+++
id = "controllers"
title = "Feedback controllers"
section = "hardware"
summary = "Run PID and hysteresis controllers with timestamped measurements and owned outputs"
aliases = ["controller", "pid", "pi", "hysteresis"]
keywords = "controller feedback pid pi pd proportional integral derivative setpoint hysteresis pwm anti-windup"
packages_any = ["service_controllers"]
+++
# Feedback controllers

Controllers compare a measurement with a setpoint and calculate an output.
The `controllers` package group includes the service and its background job.
It is enabled in the `full` flavor. Up to eight named instances are available.
Standalone names use 1 through 22 lowercase letters, digits, underscores, or hyphens.
Definitions are volatile; restore them from a startup script when required.

## Controllers owned by components

Components can create PID instances through native `controllers` ABI 1. These
share the same eight-slot registry and PID implementation. Motor velocity joints
appear under their joint names with `owner=robot`; owned names fit 23 bytes.
`controller list/status` and Python/Lua `solaros.controllers` expose their state,
including `owner`, `waiting` and `feedforward`. Output includes feedforward.

An owner supplies timestamped measurements and setpoints and calls the OS PID
synchronously on its sampling task. The generic controller job skips these
instances, so it can run alongside them without duplicate evaluation. Joint
calibration, ramping and hardware handling remain with the robot library.
`claimed` refers to outputs claimed by this controller service; a robot-owned
controller's physical motor/encoder claims are shown by `robot joint status`.

Owned instances are configured and released through their owner's API. Generic
configure, bind, feed, step, manual, auto, reset, start and delete operations are
rejected. `controller stop` (or stopping all controllers) revokes further
evaluation. A robot joint observes this on its next sample, stops its output
and latches a fault. Use robot stop/start or joint disable/enable to recover.
The owner retains the controller handle until physical cleanup succeeds.
Servo-position joints use the OS servo service and have no PID instance.

Owned instances receive setpoints through their owner rather than registering
an independently writable `controller.<name>.setpoint` parameter. Standalone
controllers retain the parameter, stream and PWM interfaces described below.

## Algorithms

`pid` provides proportional, integral, and derivative terms in parallel form.
Set unused gains to zero for P, PI, or PD operation. Defaults are `kp=1`,
`ki=0.1`, and `kd=0`, so a new PID instance starts as PI. Gains are nonnegative.
`reverse=0` means increasing output increases the measurement. Use `reverse=1`
when increasing output decreases the measurement.

The error is setpoint minus measurement, with its sign reversed for reverse
operation. The integral uses elapsed seconds and conditional integration:
integration pauses when it would drive an already saturated output farther
beyond its limit. The derivative acts on the measurement, with a first-order
filter using backward Euler and the `tau` time constant in seconds. Setpoint
changes do not create a derivative kick. A nonzero derivative gain requires
a positive `tau`.

`hysteresis` switches between `output_min` and `output_max`. With normal
direction, output becomes maximum at or below `setpoint - hysteresis/2` and
minimum at or above `setpoint + hysteresis/2`. Between these thresholds it
retains the previous output level. Reverse direction exchanges the actions.
The band width must be positive.

Setpoint and measurement use the same physical unit. Output limits and gains
must match the output adapter's unit. Values are not normalized automatically.

## Manual stepping

An instance can run without an output binding or a job:

```text
controller create test pid
controller configure test setpoint 0.8
controller configure test kp 0.5
controller configure test ki 0.2
controller step test 0.3 0.02
controller status test
controller delete test
```

`step` takes the measurement and elapsed seconds, returns the output, and
never writes an actuator. It is available only while the instance is stopped
and owns no output. Zero, negative, non-finite, or excessive elapsed times
are rejected without advancing algorithm state. `max_dt` defaults to one second.

`controller manual <name> <output>` selects a fixed output within the limits.
`controller auto <name>` resumes automatic computation. PI/PID instances with
nonzero integral gain track the manual output for a smooth first automatic
step. P and PD instances cannot compensate their proportional/derivative
terms in this way. Hysteresis resumes its two-level switching behavior.
`reset` clears algorithm history and the fault indication while stopped and
unclaimed, using `safe_output` as the initial output.

## Periodic operation

Bind a measurement source and an output before enabling an instance:

```text
controller bind <name> <stream>|manual parameter:<path>|pwm:<provider>:<output>
job start controllers
controller start <name>
controller status <name>
controller stop <name>
job stop controllers
```

`start` claims the output without writing a command. The job uses one worker
for all instances, checks them approximately every 10 ms, and steps each at
its configured period. `period_ms` defaults to 20 and accepts integer values
from 10 through 60000. Actual elapsed time is used; missed periods are not
replayed. This worker provides background control, without a hard real-time
deadline guarantee. Fast peripheral control belongs in a dedicated driver or
native application with suitable timing.

The worker accepts only scalar streams with an acquisition timestamp.
Built-in ADC streams supply this timestamp. Other providers must implement
the timestamped scalar-sample interface; ordinary scalar reads are not used
as a substitute. The provider must honor bounded read requests and report
when a cached sample was acquired, rather than when it was read.

Use `manual` as the source when a native application or script supplies
measurements. `controller feed <name> <measurement>` timestamps a new
measurement with current OS uptime. Feed a first measurement before starting
the instance, and continue feeding within `timeout_ms`. Only use this command
for a measurement acquired now. The C API also accepts the original monotonic
acquisition timestamp for delayed delivery.

Measurements must be finite, must not move backward in acquisition time, and
must not be in the future. A sample becomes stale after `timeout_ms`, which
defaults to 200. This timeout must be at least the period. `max_dt` must also
cover the period and limits the permitted interval between completed steps.

## Outputs and ownership

`parameter:<path>` writes a registered native parameter in its declared unit.
Both output limits must fit the parameter's range. The controller claims the
parameter exclusively: shell, scripting, OSC, and control-mapping setters
cannot write it while claimed. Parameter quantization remains in effect.
The algorithm reports its requested output; provider quantization or other
downstream behavior may change the applied value.

Provider removal invalidates the claim. A replacement parameter with the
same path cannot inherit it. Restart the instance explicitly after repairing
the binding. Controller setpoint parameters cannot be used as controller
output targets.

`pwm:<provider>:<output>` reserves an output through the PWM service. Its
values are pulse widths in microseconds, rounded to whole microseconds.
`frequency_hz` defaults to 50 and accepts 1 through 40000, subject to provider
support. Output limits must be nonnegative and their rounded maximum must
be shorter than the PWM period. Use `pwm providers` and `pwm status` to
discover available endpoints. Existing PWM or servo reservations can prevent
a controller from claiming an output. Motor commands and calibrated servo
angles are not output adapters in this service.

For an available GPIO2 and an ADC input on GPIO1, this example maps ADC
millivolts to PWM pulse widths. Select gains, ranges, and frequency for the
connected process:

```text
controller create level pid
controller configure level output_max 1000
controller configure level kp 0.2
controller configure level ki 0.1
controller configure level setpoint 1500
controller configure level safe_output 0
controller bind level adc1 pwm:native:2
job start controllers
controller start level
controller status level
controller stop level
job stop controllers
```

Missing or stale input, invalid arithmetic, excessive elapsed time, or a
failed output write latches a fault and stops automatic updates. The service
attempts `safe_output` immediately and retains the claim. It does not restart
automatically when a provider recovers. `status` reports the last error and
fault count. The safe write can itself fail; the status cannot prove the
physical output reached its requested value.

If closing a measurement stream fails, its handle and the output claim are
retained so that stopping the controller can retry cleanup.

`stop` writes `safe_output` and then releases ownership. Closing a PWM output
also disables its signal. A failed safe write or release retains the claim
for a later `stop` retry. Stopping the job stops all enabled instances and
attempts the same cleanup. Stop an instance successfully before deleting it
or changing its bindings or tuning.

## Configuration and inspection

```text
controller configure <name> <key> <value>
```

Keys are `setpoint`, `setpoint_min`, `setpoint_max`, `kp`, `ki`, `kd`, `tau`, `output_min`, `output_max`,
`hysteresis`, `max_dt`, `reverse` (0 or 1), `safe_output`, `period_ms`,
`timeout_ms`, and `frequency_hz`. The last three use integer values;
`timeout_ms` accepts 1 through 60000. Only `setpoint` can change while claimed.
Limits default to 0 through 1, `tau` to 0.05 seconds, hysteresis width to 1,
and safe output to 0. Safe output must lie within the limits before starting.

Each instance registers `controller.<name>.setpoint` in the parameter service,
initially accepting values from -1e20 through 1e20 in the measurement's unit.
Set `setpoint_min` and `setpoint_max` while stopped to define a useful range
for normalized control bindings. The current setpoint must fit the new range.
Shell, scripts, OSC, and existing controls can write this parameter. For example:

```text
controller configure level setpoint_min 0
controller configure level setpoint_max 3300
control create level-knob adc2 0 3300 smooth=40
control bind level-knob parameter controller.level.setpoint
job start controls
```

This maps the knob's full travel to setpoints from 0 through 3300. Configure
the range before starting `level`. Removing the instance removes its
setpoint parameter. `list` and `status` expose configuration, mode, claim and
fault state, measurement timestamp, output, error, P/I/D contributions,
saturation, and step/fault counts.

## Python and Lua

Both expose `solaros.controllers` with matching functions:

| Function | Result or behavior |
| --- | --- |
| `create(name[, kind])` | Create `pid` (default) or `hysteresis`; return status. |
| `list()` / `status(name)` | Return all status records or one record. |
| `configure(name, key, value)` | Set one numeric option. |
| `bind(name, source, target)` | Configure stream/manual input and an output adapter. |
| `feed(name, measurement)` | Publish a measurement acquired now. |
| `step(name, measurement, dt)` | Step a stopped instance; return its computed output. |
| `manual(name, output)` / `auto(name)` | Select manual or automatic mode. |
| `start(name)` / `stop(name)` | Claim and enable, or stop and release, the output. |
| `reset(name)` / `delete(name)` | Reset or remove a stopped, unclaimed instance. |

```python
import solaros

solaros.controllers.create("test")
solaros.controllers.configure("test", "setpoint", 0.8)
print(solaros.controllers.step("test", 0.3, 0.02))
print(solaros.controllers.status("test"))
solaros.controllers.delete("test")
```

Lua uses the same names and positional arguments after
`local solaros = require("solaros")`. Periodic instances belong to the OS
service and continue after the script exits; stop them explicitly when the
script owns their lifecycle.

## Native C

`solar_os_controller_math.h` provides allocation-free configuration and state
objects, reset, manual/automatic selection, and
`solar_os_controller_math_step(config, state, setpoint, measurement, dt)`.
The library depends only on the C standard library. Callers own state,
timing, output application, and fault handling. Invalid steps return false
without changing state.

`solar_os_controllers.h` exposes the named service. Timestamped stream
providers use `solar_os_stream_scalar_sample_t` with OS uptime in milliseconds.
Output and stream callbacks invoked by this service must not re-enter it.

## Quick reference

Use `controller create name pid|hysteresis`, configure the setpoint and tuning,
then call `controller step name measurement dt-seconds` for manual stepping.
Periodic instances bind a timestamped scalar stream or manual feed to an
exclusive parameter or PWM pulse output. Start `job controllers`, then start
each instance. Faults latch and attempt the configured safe output. Stop
instances to release claims; failed cleanup is retryable. Python and Lua expose
the same operations under `solaros.controllers`. Native applications can use
the allocation-free controller math library directly. Component-owned PID
instances share this registry; inspect their `owner` and `waiting` fields and
configure/release them through their owner's API.
