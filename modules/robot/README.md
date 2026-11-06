# Robot library

Optional ESP32-S3 native library implementing URDF models, transforms,
calibrated rotary joints and differential drive. The versioned C API is
[solar_os_robot_abi.h](../../include/solar_os_robot_abi.h).
Motor/encoder velocity joints configure and tick bound OS controllers through
native `controllers` ABI 1. Encoder scaling,
velocity estimation and relative position belong to the encoder service. PID,
ramping, deadband, friction feedforward and command freshness belong to the
controller service. Motor polarity, output limits and stop behavior belong to
the motor service. Standalone controllers bind those same `enc.<name>` sources
and `mot.<name>` sinks without this library. Joint configuration is applied
under endpoint/configuration claims and restored before release. No motor
feedback loop or PID implementation is linked into the ELF.
Servo-position joints own joint-to-servo conversion and calibrated limits.
Joint control works without a URDF model or display.

`controller tune` accepts standalone PID instances only; robot-owned instances
reject it. Stop the robot, tune an equivalent standalone controller, then copy
reviewed candidate gains into the URDF and reload it. Tuning does not modify
joint configuration or model files. See [Feedback controllers](../../doc/manual/controllers.md).

The model API loads expanded URDF, validates a fixed/continuous/revolute/prismatic
joint tree, accepts explicit position updates and computes frame transforms.
Transform math supports RPY construction, composition, inverse, points and
directions. Movable joints start unknown. The optional version-1 `solar_os`
block declares motor/encoder velocity controllers; loading validates their
configuration without creating controllers or claiming hardware. `robot start`
creates and enables them, waiting with outputs stopped for fresh commands.
`robot stop` releases model-owned controllers and retains the model. Stop before
reloading or unloading; failed cleanup retains ownership for retry. The optional
diff_drive component converts body velocity to owned wheel commands and
integrates measured encoder odometry. Its OS frontend publishes command,
odometry and parameter endpoints. Use `stream write` to publish one command or
periodically update several command streams; `stream echo` reads feedback.
The shell exposes `robot model load`, `set`, `transform`,
`point`, `vector`, inspection and `unload`. Python/Lua provide matching model
operations. See [Robot models and transforms](../../doc/manual/robot-models.md).

The shell exposes joint operations under `robot joint`. Continuous friction
feedforward is configured with `friction_bias_forward` and
`friction_bias_reverse`; an observed breakaway drive is a separate calibration
quantity. There is no startup boost.

## Native API

The firmware's `motion` ABI supplies generic claims and device access. Its
thread/time/memory services supply joinable workers, clocks and allocation.
Creation does not claim hardware; enable is explicit. Motor enable waits with
output stopped until the first fresh command arms the command watchdog.
Feedback and fault monitoring continue while waiting. Every client must retain
its library lease through disable, worker join and successful close. Failed
cleanup retains resources and the object for retry. Unload refuses live joints.
Command expiry stops output and returns to waiting; a fresh command resumes
without disable/enable. Hardware and feedback faults remain latched.
Counts per joint revolution are derived from PPR, the actual decoding factor
and encoder placement/gear ratio, and are exposed as read-only status.
The provider API is `robot` ABI 3 with appended model, transform and drive
functions. Joint-only clients may request `SOLAR_OS_ROBOT_JOINT_API_SIZE`;
model and drive clients must acquire a table large enough for the functions
they use. The host `motion` and `controllers` ABIs are version 1.

Native callers can keep up to four model handles, independently of the shared
shell/Python/Lua model. Close every joint, drive and model before releasing its
library lease. Model state and parser scratch use the native PSRAM allocator.
Input XML is consumed synchronously; callers can release it after load.
Transform functions support aliased input/output, normalize nonzero
quaternions and reject invalid transforms without changing the output.
Transform math and scalar streams use float32; odometry pose storage uses
double precision.

Drive I/O callbacks exchange command and status snapshots with a joint worker.
Callbacks must not block or call back into robot/controller services, and
their context must remain valid until `diff_drive_close` succeeds. See
[solar_os_robot_diff_drive.h](../../include/solar_os_robot_diff_drive.h).

## Build and install

Build against a configured ESP32-S3 firmware SDK:

```sh
python3 modules/robot/build.py --sdk-build-dir .pio/build/goouuu_esp32_s3cam
```

Without `--sdk-build-dir`, the builder configures an isolated SDK under
`.pio/robot-sdk` for `--environment` (default `esp32_s3_devkitc1_n16r8`).

Outputs are `.pio/modules/robot/robot.library.elf`, `robot.debug.elf`, and
`build.json` with artifact size/hash and verified imports. Controller math
remains in firmware. `module.toml`
integrates the provider with the signed module catalog; CMake exposes `typed_elf`.
The firmware provides native `motion` ABI 1 and `controllers` ABI 1 for motor
velocity control. Update firmware and the ELF together. The standard install path is
`modules/libraries/robot.elf` on the preferred storage volume.

See [Robot joints](../../doc/manual/robot.md) for calibration, lifecycle,
commands, Python/Lua APIs, units and limitations.
