+++
id = "servo"
title = "Servo outputs"
section = "hardware"
summary = "Control calibrated positional servos with hardware PWM"
keywords = "servo positional motor angle pulse pwm calibration pca9685 python lua"
packages_any = ["service_servo"]
+++
# Servo outputs

The servo service binds calibrated positional servos to named PWM outputs.
It is included in the `full` flavor and can be selected through the `servo`
package group in other flavors. Use `pwm providers` to discover controllers
and `pwm status` to discover their outputs.

Outputs use `provider:number`. The built-in ESP32 provider is `native`; its
output numbers are runtime-safe GPIO numbers, so `native:1` selects GPIO1.
The `pwm` group includes this backend on boards with PWM and GPIO support.
Expansion drivers can register additional PWM providers. Their output numbers
identify controller channels rather than ESP32 pins. A bare GPIO number is
accepted as shorthand for a native output.

Each named servo has explicit pulse and angle endpoints. Choose these values
from the servo's specification and the mounted mechanism's usable range.
All servo angles use radians: calibration, commands, status, streams and the
C/Python/Lua APIs. Pulse widths use microseconds and frequency uses hertz.
The default frequency is 50 Hz; supported configuration frequencies are
1 through 1000 Hz, subject to the hardware timer and servo's requirements.
Pulse endpoints must be positive, ordered, and shorter than the PWM period.
Angle endpoints must be finite and ordered. Commands outside either calibrated
range are rejected.

## Shell

```text
servo list
servo status [name]
servo open <name> <output> <min-us> <max-us> <min-rad> <max-rad> [freq-hz [inverted:0|1]]
servo set <name> <angle-rad>
servo pulse <name> <pulse-us>
servo disable <name>
servo close <name>
```

`servo list` (or bare `servo`) shows one line per open servo: its name, output,
enabled state, and owner. `servo status` adds calibration, frequency, inversion,
and the last accepted command for all open servos. `servo status <name>` shows
those details for one servo. Tab completes open servo names after `set`,
`status`, `pulse`, `disable`, and `close`.

Names contain 1 through 15 letters, digits, underscores, or hyphens. The
following example assumes an available GPIO1 and a servo calibrated for
1000 through 2000 microseconds over approximately -π/4 through π/4 radians:

```text
servo open arm native:1 1000 2000 -0.785398 0.785398
servo set arm 0
servo status arm
servo disable arm
servo close arm
```

`open` reserves the output with no pulses. The first `set` or `pulse` command
starts output. `set` linearly maps the angle endpoints to the pulse endpoints;
inversion reverses this mapping. `pulse` commands a pulse within the same
calibrated limits.

`disable` stops pulses while keeping the pin and timer reservation. Another
`set` or `pulse` resumes output. `close` stops pulses and releases the output.
These operations control the signal; they do not switch the servo's power rail.
Shell-opened servos remain reserved until explicitly closed or the OS restarts.

## Scalar stream

Opening a standalone servo also publishes `servo.<name>`. Its scalar `f32`
command input uses **radians**, matching robot geometry. It applies the same
calibration, inversion and pulse limits as the servo API. Opening a stream
sends no pulses; the first valid write starts output.

```text
servo open arm native:1 1000 2000 -0.785398 0.785398
stream write servo.arm 0.5
stream echo servo.arm --once
servo disable arm
servo close arm
```

Readback is the last commanded angle with its acquisition timestamp; it is not
measured position feedback. One stream writer is allowed alongside source
readers and echo. While that writer is attached, direct `servo set` and `pulse`
commands are rejected. Closing the writer preserves the last commanded angle
and its PWM pulses. `servo disable` stops pulses and revokes the attached
writer until it closes and opens again. `servo close` stops/releases PWM first;
if stream handles still exist, close them and retry servo close to finish
removing the named endpoint. Status exposes `input_claimed`, `stream_writable`
and `closing` through the shell and Python/Lua.

Servos opened through the native component-owned motion API, including robot
joints, expose read-only `servo.<name>` command observation. Their commands
remain restricted to the owning component API.

## Python and Lua

Both runtimes expose `solaros.servo` when the servo service is compiled:

- `list()`: return all open servo records.
- `status(name)`: return one record; fail if the name is not open.
- `open(name, output, min_us, max_us, min_rad, max_rad[, freq_hz[, inverted]])`:
  reserve a calibrated output. Frequency defaults to 50; inversion defaults
  to false. Use a boolean for inversion.
- `set(name, angle_rad)`: command an angle within the calibrated range.
- `pulse(name, pulse_us)`: command an integer pulse width within that range.
- `disable(name)`: stop pulses and retain the reservation.
- `close(name)`: stop pulses and release the reservation.

Records contain `name`, `owner`, `output`, `freq_hz`, `pulse_min_us`,
`pulse_max_us`, `angle_min_rad`, `angle_max_rad`, `inverted`, `enabled`,
`has_command`, `commanded_pulse_us`, and `commanded_angle_rad`. Command fields
are meaningful after `has_command` becomes true. They describe the last
accepted command and remain available while disabled. `pwm status` reports
the quantized hardware pulse width separately. Neither interface measures
the servo's physical position.

Python example using the calibration assumed above:

```python
import solaros

solaros.servo.open("arm", "native:1", 1000, 2000, -0.785398, 0.785398)
try:
    solaros.servo.set("arm", 0)
    print(solaros.servo.status("arm"))
finally:
    solaros.servo.close("arm")
```

Lua uses the same argument order:

```lua
solaros.servo.open("arm", "native:1", 1000, 2000, -0.785398, 0.785398)
solaros.servo.set("arm", 0)
solaros.servo.close("arm")
```

Each output belongs to the shell, Python, or Lua runtime that opened it.
Other runtimes can inspect it but cannot command or close it. Python and Lua
close their servo outputs when the interpreter exits, including after an
error or interruption. A hardware close failure is logged and retains the
reservation for retry by the owning runtime.

## PCA9685 expansion controller

The optional `pca9685` package group adds an I2C controller with 16 PWM outputs.
It is enabled in `full` on boards with expansion I2C. The expansion device name
becomes the PWM provider name, and output numbers run from 0 through 15.
With an existing `i2c0` bus and a controller at address `0x40`:

```text
expansion attach pca9685 servos0 i2c=i2c0 addr=0x40
pwm providers
servo open arm servos0:0 1000 2000 -0.785398 0.785398
servo set arm 0
servo close arm
expansion detach servos0
```

`pwm set` and `pwm pulse` also accept these outputs. Python and Lua use the
same names through `solaros.pwm` and `solaros.servo`. Attachment initializes
every channel to full-off; opening a servo does not start pulses. Closing every
servo and stopping every general PWM output permits detach. Disabled servos
still reserve their channels and the shared frequency.

All 16 channels share one prescaler. A frequency change is rejected while
another channel is active or reserved, unless both requests select the same
prescaler. Different PCA9685 devices have independent frequencies. With the
nominal 25 MHz internal oscillator, requests from 24 through 1526 Hz are
supported; the servo abstraction additionally limits requests to 1000 Hz.
The hardware has 12-bit resolution, approximately 4.88 microseconds per tick
at a requested 50 Hz. PWM status reports quantized timing.

Optional `oe=<gpio-or-controller-line>` controls the active-low output-enable
input. The driver holds it high during initialization and low after all outputs
are off. Detach raises it before shutting down the chip. Without this binding,
wire OE low. `osc_hz=<20000000..30000000>` changes the timing calculation to
match a measured internal oscillator frequency; the default is 25000000.
External-clock mode is not supported. Use an individual 7-bit address from
`0x40` through `0x77`, excluding the shared All Call address `0x70`.

Failed transfers attempt to restore the previous frequency and output values.
If restoration also fails, further commands are rejected until outputs are
closed and the controller is detached and reattached. Disable/close remain
available for cleanup. If detach cannot complete its hardware shutdown, its
context and expansion resources remain held; retry detach after restoring I2C.

The controller supplies control signals. Supply servo power from a suitable
rail and connect its ground to the controller and MCU ground.

## PWM resources

The service has a bounded pool of 16 named servos, independent of the backend's
channel capacity. PWM supports four registered providers and 32 active or
reserved outputs across those providers. Limits do not preallocate servo
objects for every controller channel.

Each provider manages its shared frequency groups. Outputs in the same group
must use compatible frequencies, including while disabled. A conflicting
request is rejected. Independent providers can use different frequencies.
An expansion provider cannot be detached while any output is active, reserved,
or executing a command. Close servo objects and stop general PWM outputs first.

The native backend has four hardware channels shared with general PWM outputs
and board peripherals using the same driver. They share one timer. A board
peripheral already using that timer at another frequency can prevent opening
a native servo.

Pulse commands update hardware duty without creating a background task.
Hardware PWM continues independently of script scheduling. Commands to an
external controller may require a bus transfer. Calls to one provider are
serialized; a transfer to one provider does not hold the global registry lock
or prevent commands to another servo on an independent provider.

Native timer resolution is 14 bits at servo frequencies: at 50 Hz, one tick is
approximately 1.22 microseconds. Other providers report their own resolution
and frequency group in `pwm status`. Pulse widths are rounded to hardware ticks.

See [GPIO and analog APIs](gpio.analog.md), [Python hardware API](python.hardware.md),
and [Lua hardware API](lua.hardware.md).

## Quick reference

Use `servo open name output min-us max-us min-rad max-rad [freq-hz [inverted]]`
with explicit calibration and a named PWM output such as native:1. Open reserves without pulses;
set and pulse start output, disable retains the reservation, close releases it.
Python and Lua expose the same operations under `solaros.servo`, plus list and
status, and close their outputs on interpreter exit. Status describes commanded
position. Providers enforce their own shared-frequency constraints. The service supports
16 servos; native PWM has four hardware channels.
