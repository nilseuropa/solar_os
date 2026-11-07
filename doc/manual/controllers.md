+++
id = "controllers"
title = "Feedback controllers"
section = "hardware"
summary = "Run PID and hysteresis controllers with timestamped measurements and owned outputs"
aliases = ["controller", "pid", "pi", "hysteresis"]
keywords = "controller feedback pid pi pd proportional integral derivative setpoint hysteresis pwm anti-windup autotune tuning relay"
packages_any = ["service_controllers"]
+++
# Feedback controllers

Controllers compare a measurement with a setpoint and calculate an output.
The `controllers` package group includes the service and its background job.
It is enabled in the `full` flavor. Up to eight named instances are available.
Standalone names use 1 through 22 lowercase letters, digits, underscores, or hyphens.
Definitions are volatile; restore them from a startup script when required.

## Setpoint streams

Each standalone controller publishes `controller.<name>.setpoint`, a scalar
`f32` command input with readback. Values use the measurement source's native
unit: for `enc.<name>`, that is output-shaft rad/s. `stream status` shows the
unit after the feedback source is bound. A setpoint command updates the target;
it does not start a controller or its background job.

For an already configured controller:

```text
stream write controller.left_pid.setpoint 0.5
stream echo controller.left_pid.setpoint --once
```

`stream write` sends one value and closes its writer. Applications can keep a
sink handle open and write native float32 values repeatedly. One writer owns
the input at a time; readback and echo remain available alongside it. The input
uses the same writer claim as `controller.<name>.setpoint` in the parameter
service, so shell configuration, parameter/OSC/control writes and a stream
producer cannot overwrite each other. Gain/limit changes, rebinding and tuning
are rejected while an input writer is attached.

Each accepted write renews `command_timeout_ms` when it is nonzero. Rejected
values do not renew it. Closing a writer leaves the last command in effect:
with timeout zero it stays latched; otherwise it expires normally. Readback
reports the last accepted command and its original acquisition timestamp, even
after expiry stops the output. Controller status reports the current waiting
state and active setpoint separately. Close open stream handles before deleting
the controller. Setpoint streams are command inputs/readback, not PID feedback
sources or targets for another PID instance.

Python and Lua use the existing stream API. For example, Python can open
`solaros.streams.open("controller.left_pid.setpoint", {"direction": "sink"})`
and write `struct.pack("<f", value)` through `solaros.streams.write(handle, data)`.
Lua uses the same sink option and `string.pack("<f", value)`. Close the handle
when the producer stops. The ordinary setpoint parameter and convenience
`controller configure <name> setpoint <value>` remain available when no stream
writer owns the input.

## Controllers owned by components

Components can create PID instances through native `controllers` ABI 1. These
share the same eight-slot registry and PID implementation. Motor velocity joints
appear under their joint names with `owner=robot`; owned names fit 23 bytes.
`controller list/status` and Python/Lua `solaros.controllers` expose their state,
including `owner`, `waiting` and `feedforward`. Output includes feedforward.

Motor velocity joints bind the same calibrated encoder sources and motor sinks
available to standalone controllers. Encoder scaling, filtering and relative
position belong to the encoder service. PID, ramping, deadband, friction
feedforward and command freshness belong to the controller service; motor
polarity, drive limits and stop behavior belong to the motor service. A joint
supplies commands and ticks its owned OS controller on its sampling task.
The generic controller job skips owned instances to avoid duplicate stepping.
`claimed` includes the bound device streams. No motor feedback/control loop
implementation is linked into the robot library.

Owned instances are configured and released through their owner's API. Generic
configure, bind, feed, step, manual, auto, reset, start, tune and delete operations are
rejected. `controller stop` (or stopping all controllers) revokes further
evaluation and stops/releases its bound endpoints. A robot joint observes
this on its next sample and latches a fault. Use robot stop/start or joint disable/enable to recover.
The owner retains the controller handle until physical cleanup succeeds.
Servo-position joints use the OS servo service and have no PID instance.

Owned instances receive setpoints through their owner rather than registering
an independently writable `controller.<name>.setpoint` parameter. Standalone
controllers retain the parameter, stream and PWM interfaces described below.
Owned instances expose `controller.<name>.setpoint` as read-only command
observation. Stream writers cannot bypass the robot API or its lifecycle.
Robot startup claims endpoint configuration, applies URDF settings, and restores
the previous settings before releasing the endpoints. Failed cleanup retains
ownership for a later stop retry.

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
unclaimed, using `safe_output` as the initial output. It also clears tuning results.

## Standalone PID tuning

```text
controller tune <name> [noise-floor [points [timeout-ms]]]
controller status <name>
controller stop <name>
```

`tune` starts an automatic experiment on a stopped, unclaimed standalone
PID controller. It works without the robot library installed. Component-owned
controllers, including `owner=robot`, reject tuning without changing their state.
Hysteresis instances cannot be tuned.

Configure direction, permitted output limits, `safe_output`, measurement
timing, and the permitted measurement range (`setpoint_min/max`) first.
**The current setpoint is ignored**; the tuner chooses its own operating points.
It measures a one-second baseline, increases drive in 2% increments of the
available output span until motion is detected, then measures several
five-second step responses above the detected breakaway output. Between steps
it returns to `safe_output` and waits for the baseline to recover.

The default is four step-response points. Optional `points` accepts 3 through
12. `noise-floor` is the minimum meaningful response in feedback units, not
motor power. Omitted or zero selects 0.02; measured baseline noise can raise
the threshold to six standard deviations. The total timeout defaults to
120000 ms and accepts 1000 through 300000. The former relay `band/cycles`
arguments now select the response floor and number of step-response points.

One invocation tests one direction: increasing output from `safe_output`, or
decreasing output if increasing output has no headroom or the measurement
range excludes it. To characterize the opposite motor direction, use a separate
run with output limits that permit only that direction. Output stays within
the configured limits. Feedback outside the measurement range, with a noise
allowance, stops the test; this is a monitored limit, not a guarantee against
transient mechanical overshoot.

Use `tune` instead of `start`. The shell starts the `controllers` worker if
needed for a bound controller. API callers start the job separately.
Tuning claims the configured parameter, PWM output, or scalar stream sink
and uses the normal timestamped stream or `feed` input. It makes no output
write until a valid measurement is processed. Continue supplying fresh samples;
cached timestamps do not count as new identification samples. The ordinary
feedback timeout and `max_dt` checks remain active. Configuration and setpoint
writes, including parameter/control/OSC writes, are rejected during tuning.

Each response estimates process gain, delay and time constant using its
10% and 63.2% crossings. Flat, unresolved, wrongly signed, or still drifting
responses fail with a specific reason. The per-point PID candidates are
averaged, then exercised privately at 80% of each measured speed change from
baseline, leaving output headroom for corrections. Each
target must track continuously for one second within the larger of twice the
response floor and 5% of its change from baseline, within ten seconds per
target. Validation uses the PID output directly, without command ramps or
friction feedforward. Configured gains, setpoint and bias/ramp settings remain
unchanged. Status shows phase, point counts, test setpoint, breakaway output,
noise and fitted response data. A test that cannot produce motion within its
output limit fails with `no-motion-within-output-limit`.

On completion, the service writes `safe_output`, releases the output, and
reports candidate `kp`, `ki`, `kd`, and `tau` in `status`. **Configured gains
are never changed automatically.** A failed final cleanup invalidates the
candidate and retains the claim for `stop` to retry. `stop` aborts a running
experiment and performs the same safe-output cleanup. Input, timing, or I/O
faults stop updates and retain claims for explicit cleanup, as in periodic
operation. Results are volatile and remain visible until reset, deletion, or
another tuning attempt.

The step-response identification and IMC PID equations are adapted from
Tortuga's `PidTuner`. The smoothing time is at least the fitted time constant
and four sample intervals. The derivative filter is at least one sample
interval or one tenth of the fitted delay. These are heuristic candidates;
validation covers the tested targets and load, not every operating condition.

With no source or output binding, `tune` instead advances through
`controller step` or `solaros.controllers.step`. Each call returns the next
test output, or `safe_output` on completion. No background job or actuator
write occurs. The caller supplies acquisition timing, applies the returned
output, and stops the physical actuator on failure, abort, or completion.
After this experiment ends, further `step` calls are rejected until `reset`
or an explicit periodic `start`; this prevents an accidental return to the
old PID loop.

To tune a robot wheel independently, stop the robot, configure the encoder and
motor through their OS services, and bind a standalone controller as below.
Use the same calibration, filtering and permitted drive/speed limits as the
joint. Copy reviewed candidate `kp`, `ki`, `kd` and `tau` into the URDF,
export and reload it, then explicitly start the robot. Tuning does not edit
model files.

## Standalone encoded motor

An attached motor and encoder can run closed-loop velocity control without the
robot library or a script. Attachment defines wiring and counting mode;
service configuration defines calibration and policy. For Solbot's already
attached `left` motor and `left_enc` encoder:

```text
encoder configure left_enc ppr 70
encoder configure left_enc gear_ratio 30
encoder configure left_enc polarity 1
encoder configure left_enc velocity_tau 0.1
motor configure left max_drive 0.2
motor configure left polarity 1
controller create left_pid pid
controller configure left_pid output_min -0.2
controller configure left_pid output_max 0.2
controller configure left_pid kp 0.04
controller configure left_pid ki 0.03
controller configure left_pid setpoint 0.3
controller bind left_pid enc.left_enc stream:mot.left
job start controllers
controller start left_pid
controller status left_pid
controller stop left_pid
```

The setpoint and feedback are output-shaft rad/s. Output is normalized motor
drive, not rad/s. Both PID output limits must fit the motor's `max_drive` and
motor bindings require `safe_output=0`. The encoder must have a positive PPR.
Stop the robot first if it owns these devices.

For tuning, leave the controller stopped and use:

```text
controller tune left_pid
controller status left_pid
```

The preceding 0.2 drive cap is an example control limit, not a useful tuning
envelope for a motor whose breakaway output is already 0.2. Tuning needs
permitted headroom above breakaway to measure distinct operating speeds.
Choose that envelope for the assembly before running the experiment.

The controller job executes the experiment, stops output and releases the
streams on completion. `controller stop left_pid` aborts it. Existing gains
remain unchanged. No Python/Lua sampling loop is required.

`acceleration_limit` limits setpoint change per second; zero disables ramping.
`velocity_deadband` maps small requested velocities to zero. Both default to
zero. `friction_bias_forward` and `friction_bias_reverse` default to zero and
supply continuous direction-dependent feedforward within the output limits.
Feedforward participates in PID anti-windup. Zero command removes feedforward
but continues feedback control. Manual output mode does not add feedforward.

`command_timeout_ms=0` (default) keeps a standalone setpoint until it changes
or the controller stops. A positive timeout requires fresh setpoint writes,
including through `controller.<name>.setpoint`. Expiry resets the ramp/PID,
stops output and enters `waiting`, while continuing feedback checks and
retaining claims. A fresh setpoint resumes operation. It is distinct from
`timeout_ms`, which limits feedback age. Both timeouts, when enabled, must
cover the controller period. Relay tuning uses its own bounded experiment
timeout instead of requiring repeated setpoint writes.

## Periodic operation

Bind a measurement source and an output before enabling an instance:

```text
controller bind <name> <stream>|manual parameter:<path>|pwm:<provider>:<output>|stream:<sink>
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

`stream:<sink>` claims a scalar `f32` sink exclusively for the controller's
lifetime. `stream:mot.<name>` writes normalized signed motor drive through
the motor service. Encoder sources `enc.<name>` remain open for the same
lifetime, locking calibration and preventing reset/detach. Ordinary motor
commands and competing output opens are rejected while claimed. Motor sinks
check local feedback/command deadlines after status acquisition and before
applying a new drive. Closing the sink coasts or brakes according to its
stop policy. Failed closes retain handles for retry.


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
a controller from claiming an output. Calibrated servo angles are not output adapters in this service.
Use a motor stream for normalized drive.

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
`timeout_ms`, `frequency_hz`, `command_timeout_ms`, `acceleration_limit`,
`velocity_deadband`, `friction_bias_forward`, and `friction_bias_reverse`.
Period, timeouts and frequency use integer values;
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
| `tune(name[, noise_floor[, points[, timeout_ms]]])` | Start automatic standalone PID tuning; return status. Defaults: 0, 4, 120000. |
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

Status records include `tune_state` (`idle`, `running`, `complete`, `aborted`,
or `failed`), `tune_reason`, `tune_phase`, `tune_point`, `tune_points`,
`tune_test_setpoint`, `tune_drive`, `tune_breakaway`, `tune_noise`,
`tune_process_gain`, `tune_dead_time`, `tune_time_constant`, `tune_elapsed`, and
candidate `tune_kp`, `tune_ki`, `tune_kd`, `tune_tau`. Candidate fields are
valid only when `tune_state` is `complete`.
Legacy `tune_cycles`/`tune_required_cycles` count identified response points;
relay-only `tune_period`, `tune_amplitude` and `tune_ultimate_gain` remain zero.

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
exclusive parameter, PWM pulse output, or scalar stream sink.
For encoded motors, configure `encoder` and `motor`, then bind
`enc.<encoder>` to `stream:mot.<motor>`. This path needs no robot library. Start `job controllers`, then start
each instance. Faults latch and attempt the configured safe output. Stop
instances to release claims; failed cleanup is retryable. Python and Lua expose
the same operations under `solaros.controllers`. Native applications can use
the allocation-free controller math library directly. Component-owned PID
instances share this registry; inspect their `owner` and `waiting` fields and
configure/release them through their owner's API.
Use `controller tune name [noise-floor [points [timeout-ms]]]` on a stopped standalone
PID to report candidate gains without applying them. Inspect results with
`status`, abort with `stop`, and use `reset` before resuming manual stepping.
Robot-owned controllers reject tuning; copy reviewed standalone results into
the URDF and reload it.
