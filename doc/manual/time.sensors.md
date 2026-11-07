+++
id = "time.sensors"
title = "Time, battery, and environment APIs"
section = "hardware"
summary = "Read clocks, battery state, environment, and IMU measurements"
aliases = ["sensors"]
keywords = "python lua time clock date timezone ntp uptime rtc alarm timer schedule battery sensor temperature humidity environment imu mpu6050 acceleration gyro stream frames"
packages_any = []
+++
# Time, battery, and environment APIs

SolarOS distinguishes uptime, UTC, and configured local time. Sensor and
battery values exist only when the board and firmware provide their services.
`solaros.rtc` exposes optional alarm/timer hardware; `solaros.schedule` provides
portable alarms and recurring actions whether or not that hardware exists.

## Use the clock

```python
import solaros

print(solaros.time.uptime())
if solaros.time.is_valid():
    print(solaros.time.datetime())
```

Do not label data with wall-clock timestamps until the RTC or NTP-derived time
is valid. Use uptime for monotonic intervals.

## Read installed sensors

Inspect `solaros.battery.status()` and `solaros.sensors.list()` rather than
assuming a fixed voltage, temperature, or humidity source. Read the default or
a named provider with `solaros.sensors.environment()`, `temperature()`, or
`humidity()`. On Pi0cket CL-32,
`battery0` reads the voltage measured by the integrated AVR in 25 mV steps and
reports its USB-power and charging states directly.
On the Waveshare ESP32-S3-SIM7670G-4G target, the MAX17048 reports voltage and
state of charge but has no charger-status input. `Charging` therefore remains
unknown. The battery service can still estimate `Power` from configured voltage
thresholds and monitor trends; that estimate is not direct charger telemetry.

## Quick reference

solaros.time provides uptime_ms, uptime, datetime, utc_datetime, set_datetime,
set_utc_datetime, utc_to_local, local_to_utc, is_valid, timezone,
set_timezone, and ntp_sync. solaros.battery.status and the solaros.sensors
functions are package-gated.

`set_timezone()` accepts conventional fixed UTC offsets such as `UTC-8` and
`UTC+5:30`. Fixed offsets do not apply daylight-saving transitions. Other
accepted timezone expressions use POSIX TZ syntax and its POSIX sign
convention. SolarOS does not include the IANA timezone database;
`Europe/Berlin` is a built-in daylight-saving alias.

## IMU samples and streams

The IMU service exposes each attached device as `imu.<device>`, a shared source.
It reads through the existing driver when a consumer requests a sample; opening
the stream does not create a periodic sampling task. The chip still measures
at its configured hardware rate. Readers obtain current measurements, not a
lossless history, and multiple readers may sample at different times.

For an MPU6050 on a named I2C bus:

```text
expansion attach mpu6050 imu0 i2c=i2c0 addr=0x68
imu list
imu sample imu0
stream status imu.imu0
stream echo imu.imu0 --once
stream echo imu.imu0 --rate 20
```

The driver accepts addresses `0x68` and `0x69`. Add `irq=<pin>` only when INT
is wired to an available native GPIO. The optional input is active high and
latched until status is read. Without it, the driver polls status over I2C.
The default measurements are 100 Hz, ±2 g and ±250 degrees/s, with the hardware
low-pass filter enabled. Reported units are m/s² and rad/s. The driver neither
estimates orientation nor subtracts gravity or gyro bias.

`stream echo` prints one CSV record containing an acquisition timestamp, a
validity mask, acceleration, angular velocity and optional orientation. A zero
orientation field is not an orientation estimate: validity bit 0 means
acceleration, bit 1 angular velocity, and bit 2 orientation. IRQ-free MPU6050
timestamps mark host sample read completion, without a hardware clock or FIFO
history. Other IMU providers retain their own sample timestamps.

Native byte reads return one complete 52-byte `imu-v1-le` record. It contains
little-endian `uint64 timestamp_us`, `uint32 valid`, then ten IEEE float32
values: acceleration x/y/z, gyro x/y/z, and orientation w/x/y/z. There is no
padding. Buffers smaller than 52 bytes fail without acquiring a sample. Open
readers prevent detachment; close them before `expansion detach <device>`.
Python and Lua can also read structured values with `solaros.imu.sample()`.

URDF device/frame associations belong to the [robot model](robot-models.md).
The IMU source carries no frame ID and remains usable without the robot library.
