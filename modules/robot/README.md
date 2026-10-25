# Robot library

Optional ESP32-S3 native library implementing calibrated rotary joints. The
versioned C API is [solar_os_robot_abi.h](../../include/solar_os_robot_abi.h).
Motor/encoder velocity joints own PPR, gearbox ratio, encoder placement,
motor/encoder polarity, velocity estimation, ramping, command deadband,
directional friction feedforward and PI/PID state.
Servo-position joints own joint-to-servo conversion and calibrated limits.
There is no URDF or graphics dependency.

The shell exposes joint operations under `robot joint`. Continuous friction
feedforward is configured with `friction_bias_forward` and
`friction_bias_reverse`; an observed breakaway drive is a separate calibration
quantity. This version does not implement a startup boost.

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
The provider API is `robot` ABI 3; the generic host `motion` ABI remains 1.

Build against a configured ESP32-S3 firmware SDK:

```sh
python3 modules/robot/build.py --sdk-build-dir .pio/build/goouuu_esp32_s3cam
```

Without `--sdk-build-dir`, the builder configures an isolated SDK under
`.pio/robot-sdk` for `--environment` (default `esp32_s3_devkitc1_n16r8`).

Outputs are `.pio/modules/robot/robot.library.elf`, `robot.debug.elf`, and
`build.json` with artifact size/hash and verified imports. The builder links
portable controller math from the shared firmware source. `module.toml`
integrates the provider with the signed module catalog; CMake exposes `typed_elf`.
The firmware must provide native `motion` ABI 1. The standard install path is
`modules/libraries/robot.elf` on the preferred storage volume.

See [Robot joints](../../doc/manual/robot.md) for calibration, lifecycle,
commands, Python/Lua APIs, units and limitations.
