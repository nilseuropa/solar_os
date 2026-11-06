+++
id = "robot"
title = "Robot joints"
section = "hardware"
summary = "Use the optional external robot library for calibrated servo and motor/encoder joints"
keywords = "robot joint servo velocity motor encoder ppr gearbox ratio polarity pi pid breakaway friction deadband"
packages_any = ["service_robot"]
+++
# Robot joints

The optional `robot` native library provides rotary servo-position and
motor/encoder-velocity joints through the shell, Python and Lua. Velocity joints
configure the OS encoder, motor and controller services; standalone controllers
can use those services without the robot library. Previous device settings are
restored on disable or stop.
Robot joints work without URDF or a display. The same library also provides
[URDF models, transforms and differential drive](robot-models.md). Native
libraries require ESP32-S3, executable PSRAM and the native-module service.

An expanded URDF can also declare motor/encoder velocity controllers in its
`solar_os` block. Load the model, then use `robot start` to create and enable
those controllers and `robot stop` to close them. They use the same calibration
and command watchdog as manually created joints. See [model controller bindings](robot-models.md#solaros-extension).

Enabled motor joints appear under their joint names in `controller list` and
`controller status <joint>`, with `owner=robot`. Their controller state and gains
are also visible through `solaros.controllers`. Configure them through the joint
API or URDF while disabled. `robot stop` removes the model's controller instances
after hardware cleanup succeeds. A failed stop retains the instance for retry.
`controller stop <joint>` stops and releases the bound endpoints and causes a
joint fault. Recover with joint
disable/enable, or model stop/start. Servo-position joints use the OS servo
service and do not create a PID instance.

Install `robot` from a compatible signed module catalog before creating a
joint. Creating the first joint loads the installed library on demand; it does
not download code. Loading, creating, configuring and binding do not energize
outputs. `enable` explicitly claims hardware and starts control. Python/Lua
bindings are included on supported native-module builds.

## Calibration

Each joint declares this configuration, applied through shared OS services:

- `ppr`: A-channel pulse periods per encoder-shaft revolution, independent of
  decoding. Configure this explicitly for motor joints.
- `gear_ratio`: motor/input revolutions per output/joint revolution. Configure
  this explicitly for motor joints, including 1 for direct drive.
- `motor_polarity`: +1 or -1, mapping positive joint demand to actuator direction.
- `encoder_polarity`: +1 or -1, mapping encoder counts to positive joint motion.
- `encoder_on_output`: 0 for a motor/input-shaft encoder (default), 1 for a
  gearbox-output encoder.
- `position_zero`: joint coordinate in radians at the initial reference.

The encoder provider reports its actual x1/x2/x4 decoding factor. Counts per
joint revolution are `ppr * decoding * gear_ratio` for a motor-shaft encoder,
or `ppr * decoding` for an output-shaft encoder. Do not enter already decoded
counts or output-shaft counts as PPR. Gearbox ratio and quadrature decoding
are applied once. Both polarities must be checked for the mounted mechanism;
they are independent of each other and of the positive direction in a driver.

`encoder_counts_per_rev` is read-only status derived from these parameters.
It cannot be configured independently. The value remains pending until the
encoder's decoder mode has been sampled. For example, PPR 70, x1 decoding and
a ratio of 30 derive 2100 counts per joint revolution. A manually measured
full revolution validates this result; correct PPR, ratio or encoder placement
when the measurement disagrees with it.

Velocity uses acquisition timestamps and radians per second. Signed 32-bit
encoder wrap is accumulated in a 64-bit relative count, assuming fewer than
2^31 counts between samples. `velocity_tau` is the first-order low-pass time
constant in seconds; 0 disables filtering. The default is 0.1 seconds.

Motor joints expose relative displacement since enable in `position`, but
`position_valid` and `referenced` remain false because there is no homing or
absolute position reference. Servo joints expose
`commanded_position`; their measured position and velocity remain invalid.
Motor drive is normalized demand, not torque or measured effort.

## Shell

Joint operations live under `robot joint`.

```text
robot joint list
robot joint create <name> velocity|servo
robot joint configure <name> <key> <value>
robot joint motor <name> <motor> <encoder>
robot joint servo <name> <provider> <output> <min-us> <max-us> <min-rad> <max-rad> [freq-hz]
robot joint command <name> <target>
robot joint enable <name>
robot joint status <name>
robot joint disable <name>
robot joint close <name>
```

For an encoder calibrated at 70 A-channel periods per shaft revolution and a
30:1 gearbox, prepare a joint using already registered devices:

```text
robot joint create wheel velocity
robot joint motor wheel wheel_motor wheel_encoder
robot joint configure wheel ppr 70
robot joint configure wheel gear_ratio 30
robot joint configure wheel motor_polarity 1
robot joint configure wheel encoder_polarity 1
robot joint configure wheel max_drive 0.2
robot joint status wheel
```

This only configures the joint. `enable` first claims the motor, applies the
configured coast/brake behavior, claims the encoder reference, acquires a
baseline sample and starts the loop. Without a fresh queued command, it keeps
the configured coast/brake behavior while waiting for the first command.
Feedback and device fault monitoring continue, but the command watchdog is
not armed and the joint does not regulate velocity yet. An expired queued
velocity command is discarded on enable. The first fresh `command` arms the
watchdog, including an explicit zero command. Refresh targets in radians/second
before their expiry. `disable` stops control and releases the devices after
applying the configured stop behavior. `close` disables and destroys the joint.

For a positional servo, `servo` specifies its input-shaft calibration. The
adapter converts joint position to servo-shaft angle as
`motor_polarity * (target - position_zero) * gear_ratio`. Supply a fresh
position command in radians before enabling. Servo enable reserves PWM without
pulses; the first control update commands the position. Measured joint position
is unavailable without a feedback sensor. Disabling closes PWM and stops the
signal; it does not remove actuator power.

## Control and lifecycle

Velocity joints use the OS PI/PID controller service with calibrated encoder
feedback, setpoint ramping and normalized motor output.
`motor_polarity` is applied after the controller; `encoder_polarity` is applied
to feedback. Saturation, anti-windup and filtered derivative use the common
controller implementation. Joint control runs independently of `job controllers`,
which can run other controller instances alongside it. Timing depends on device
access and OS scheduling, without a hard real-time guarantee.

Motor claims exclude ordinary drive, coast, brake and clear writes and prevent
detach. Encoder reference claims permit shared reads but prevent reset and
detach. Servo outputs reserve their PWM output until disabled.

Command expiry stops the output, resets the controller and enters `waiting`
while retaining device claims and feedback monitoring. A fresh command resumes
control immediately; disable/enable is not required. A failed stop latches a
fault instead. Failed/stale/nonmonotonic feedback, excessive scheduling gaps,
blocking motor faults, arithmetic failure and output failures latch a fault.
The worker attempts the configured stop behavior and exits. Claims remain held
until `disable` or `close` completes cleanup. Failed stopping retains both motor
and encoder claims for retry. A failed stop command does not prove the physical
actuator stopped. Repair the cause and disable successfully before re-enabling.

Python/Lua-owned joints are closed on interpreter teardown. Failed cleanup keeps
the joint and its library lease available for shell inspection and retry.
Shell-owned joints persist until explicitly closed or restart. After the first
command, expiry also applies when a script exits or a transport disconnects. Library unload is
refused while a joint still holds a lease.

## Configuration and state

Configuration is accepted only while the joint owns no hardware and has no
worker. Changes invalidate any queued command. Keys include `ppr`,
`gear_ratio`, `motor_polarity`, `encoder_polarity`, `encoder_on_output`,
`position_zero`, `position_min`, `position_max`, `velocity_limit`,
`acceleration_limit`, `max_drive`, `velocity_tau`, `friction_bias_forward`,
`friction_bias_reverse`, `velocity_deadband`, `kp`, `ki`, `kd`, `tau`,
`period_ms`, `command_timeout_ms`, `feedback_timeout_ms`, `max_gap_ms` and `brake`.
`brake=0` selects coast (default); `brake=1` requires a brake-capable motor.

Defaults are a 20 ms period, 300 ms command timeout, 100 ms feedback timeout,
200 ms maximum scheduling gap, 10 rad/s velocity limit, 10 rad/s² setpoint
acceleration limit and 0.2 maximum normalized drive. PI defaults are `kp=0.02`,
`ki=0.01`, `kd=0`, with derivative filter `tau=0.05` seconds. These are initial
settings, not gains tuned for a particular mechanism. Configure both PPR and
gearbox ratio before enabling a velocity joint. Unconfigured motor joints report zero for PPR and gearbox
ratio; zero cannot be configured as a valid PPR or ratio. A direct drive using
PPR-based conversion still requires an explicit ratio of 1.

Velocity targets use radians/second; they are not normalized motor-drive
commands. `kp` converts velocity error to normalized drive, and `ki` integrates
that error over seconds. For example, a stationary joint commanded to 0.5 rad/s
with `kp=0.02` initially requests only 0.01 proportional drive. Such a request
can fall below the driver's output deadband or the mechanism's breakaway drive.
The default gains do not guarantee rotation for a particular motor. Configure
gains while disabled, keep a bounded `max_drive`, and verify encoder motion
and overshoot before using the joint continuously.

The command timeout defaults to 300 ms and starts on the first command.
For interactive shell checks, configure `command_timeout_ms` while disabled
to allow enough time for typing. A target is valid only for that duration;
continuous operation requires refreshed commands from a script or another
client. After ordinary command expiry, a fresh command resumes control from
the stopped state. Configuration changes still require `disable`. Hardware,
feedback and scheduling faults require successful disable/recovery before
re-enabling. `status` reports
the command timeout, whether the watchdog is armed, and the recovery command.

The joint owns actuator calibration and the velocity-command deadband.
`friction_bias_forward` and `friction_bias_reverse` are nonnegative normalized
feedforward magnitudes for joint-positive and joint-negative motion, each at most
`max_drive`. Both default to zero. The sign of the ramped velocity setpoint
selects a continuous drive bias; a zero setpoint adds none. Calibrate this bias
while the mechanism is rotating at the intended speed and load. Feedback may
reduce or cancel it. PID output bounds account for compensation so the final
motor command respects `max_drive` and
anti-windup remains effective. Polarity is applied after summing these terms.

**Breakaway drive** is the normalized actuator command at which a stationary
mechanism begins to rotate. For example, a breakaway drive of 0.15 means
rotation starts at about 15% of full-scale drive in that direction. This is an
observed threshold; it does not measure power, torque, or the drive required
to sustain rotation. It can differ by direction and load. There is no separate
breakaway threshold or startup boost. A breakaway
observation alone does not calibrate `friction_bias_forward` or
`friction_bias_reverse`.

`velocity_deadband` is a command tolerance in rad/s, default zero and strictly
less than `velocity_limit`. Commands with absolute value at or below this
tolerance become a zero target before ramping. Encoder feedback is never
zeroed by this tolerance. A zero velocity target remains closed-loop and may
apply corrective drive; use `disable` to stop and release the motor. Mechanical
backlash and servo pulse deadband are not compensated.

Periods accept 10 through 1000 ms. Timeouts and maximum gaps accept the period
through 60000 ms. PPR, ratio, velocity and acceleration limits must be positive;
maximum drive is greater than zero and at most 1. Position limits are radians
and constrain servo targets. Velocity targets outside `velocity_limit` are
rejected. This version does not control bounded-travel motor position or
provide trajectory planning, homing, force control, or torque measurement.

`status` reports configuration, state (`ready`, `active`, `waiting`, `fault`), claims,
command expiry, acquisition time, raw count, decoding, validity, target,
rate-limited setpoint, velocity, relative position, commanded servo position,
last output command, friction feedforward, P/I/D contributions, saturation and counters. The library
error uses `solar_os_native_service_result_v1_t`, with zero for success.

## Python and Lua

Both runtimes expose `solaros.robot`:

| Function | Behavior |
| --- | --- |
| `joints()` / `joint_status(name)` | Return all records or one record. |
| `joint_create(name[, kind])` | Create `velocity` (default) or `servo`; return status. |
| `joint_configure(name, key, value)` | Set one numeric option while inactive. |
| `joint_bind_motor(name, motor, encoder)` | Select named devices for a velocity joint. |
| `joint_bind_servo(name, provider, output, min_us, max_us, min_rad, max_rad[, frequency_hz])` | Set input-shaft servo calibration; frequency defaults to 50. |
| `joint_command(name, target)` | Refresh a velocity or position command. |
| `joint_enable(name)` / `joint_disable(name)` | Activate or stop and release hardware. |
| `joint_close(name)` | Stop, release and destroy a joint. |

```python
import solaros

solaros.robot.joint_create("wheel")
try:
    solaros.robot.joint_bind_motor("wheel", "wheel_motor", "wheel_encoder")
    solaros.robot.joint_configure("wheel", "ppr", 70)
    solaros.robot.joint_configure("wheel", "gear_ratio", 30)
    solaros.robot.joint_enable("wheel")
    for _ in range(10):
        solaros.robot.joint_command("wheel", 1.0)
        solaros.time.sleep_ms(50)
    solaros.robot.joint_disable("wheel")
finally:
    solaros.robot.joint_close("wheel")
```

This example commands movement and assumes the specified calibration matches
the connected mechanism. Lua uses the same names and arguments after
`local solaros = require("solaros")`.

Native API and build instructions are in the
[robot library README](../../modules/robot/README.md).

## Quick reference

Install the optional robot library, create a joint, configure PPR, gearbox ratio
and polarities, and bind named motor/encoder or calibrated servo outputs.
Counts per joint revolution are derived and exposed as read-only status.
Enable explicitly and refresh commands within their timeout. Expiry stops output
and waits for a fresh command; hardware and feedback faults require recovery.
Status distinguishes commanded, relative and measured quantities. Disable stops
outputs before releasing hardware; failed cleanup is retryable. Close joints before
unloading the library. Enabled motor joints also appear in `controller list`
under their joint names. Python and Lua expose the same operations in solaros.robot.
