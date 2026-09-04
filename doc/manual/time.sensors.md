+++
id = "time.sensors"
title = "Time, battery, and environment APIs"
section = "hardware"
summary = "Read clocks, battery state, temperature, and humidity"
aliases = ["sensors"]
keywords = "python lua time clock date timezone ntp uptime rtc alarm timer schedule battery sensor temperature humidity environment"
packages_any = []
+++
# Time, battery, and environment APIs

SolarOS distinguishes uptime, UTC, and configured local time. Sensor and
battery values exist only when the board and firmware provide their services.
`solaros.rtc` exposes optional alarm/timer hardware; `solaros.schedule` provides
portable alarms and recurring actions whether or not that hardware exists.

## Check time integrity

```python
import solaros

print(solaros.time.uptime())
if solaros.time.is_valid():
    print(solaros.time.datetime())
```

Do not label data with wall-clock timestamps until the RTC or NTP-derived time
is valid. Use uptime for monotonic intervals.

## Read installed sensors

Inspect `solaros.battery.status()` and `solaros.sensors.environment()` rather
than assuming a fixed voltage, temperature, or humidity source. On CL-32,
`battery0` reads the voltage measured by the integrated AVR in 25 mV steps and
reports its USB-power and charging states directly.

## Quick reference

solaros.time provides uptime_ms, uptime, datetime, utc_datetime, set_datetime,
set_utc_datetime, utc_to_local, local_to_utc, is_valid, timezone,
set_timezone, and ntp_sync. solaros.battery.status and
solaros.sensors.environment are package-gated.

`set_timezone()` accepts conventional fixed UTC offsets such as `UTC-8` and
`UTC+5:30`. Fixed offsets do not apply daylight-saving transitions. Other
accepted timezone expressions use POSIX TZ syntax and its POSIX sign
convention. SolarOS does not include the IANA timezone database;
`Europe/Berlin` is a built-in daylight-saving alias.
