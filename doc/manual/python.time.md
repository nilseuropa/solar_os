+++
id = "python.time"
title = "Python time and scheduling API"
section = "api"
summary = "Time and scheduling: time, rtc, schedule"
keywords = "python solaros api time time rtc schedule"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python time and scheduling API

[API overview](python.md) · [Lua time and scheduling](lua.time.md)

## Datetime values

Datetime dictionaries use this shape:

```python
{
    "year": 2026,
    "month": 6,
    "day": 19,
    "hour": 12,
    "minute": 30,
    "second": 0,
    "weekday": 5,
    "clock_integrity": True,
}
```

Datetime setters and converters accept either such a dict or positional values:

```python
solaros.time.set_datetime(2026, 6, 19, 12, 30, 0)
solaros.time.set_datetime({"year": 2026, "month": 6, "day": 19, "hour": 12, "minute": 30})
```

## `solaros.time`

Time functions use the SolarOS RTC/time service.

- `uptime_ms()`: return uptime in milliseconds.
- `sleep_ms(ms)`: delay for up to one hour while remaining responsive to script cancellation.
- `uptime()`: return formatted uptime text.
- `datetime()`: return the local wall-clock datetime.
- `utc_datetime()`: return UTC datetime.
- `set_datetime(datetime)`: set the local wall-clock datetime.
- `set_utc_datetime(datetime)`: set the UTC wall-clock datetime.
- `utc_to_local(datetime)`: convert UTC datetime to local time.
- `local_to_utc(datetime)`: convert local datetime to UTC.
- `is_valid(datetime)`: validate a datetime.
- `timezone()`: return `{"name": ..., "posix": ...}`.
- `set_timezone(timezone)`: set timezone by SolarOS-supported name or POSIX TZ string.
- `ntp_sync([server[, timeout_ms]])`: sync wall-clock time from NTP and return `{"utc": ..., "local": ...}`. `server` defaults to `pool.ntp.org`, `timeout_ms` to `40000`. With less than 40000, a sync whose first packet is lost fails before lwIP's retry.

Example:

```python
import solaros

print("uptime", solaros.time.uptime())
print("local", solaros.time.datetime())

if solaros.wifi.status()["has_ip"]:
    print(solaros.time.ntp_sync())
```

## `solaros.rtc`

Direct access to optional RTC alarm and countdown hardware:

- `status()`: return availability, provider, capabilities, interrupt GPIO and active level, and current slot owners.
- `set_alarm(hour, minute[, second[, day[, weekday]]])`; `clear_alarm()`.
- `set_timer(seconds[, repeat])`; `clear_timer()`.
- `pending()`; `ack(mask)`, using `INTERRUPT_ALARM` and `INTERRUPT_TIMER`.

Direct slots are leased to the Python runtime and released when it exits. A
busy error means the scheduler or another client owns that hardware slot.

## `solaros.schedule`

Named schedules remain available after the creating script exits:

- `list()` returns entry dictionaries.
- `add_in(name, seconds[, action[, value[, persistent]]])`.
- `add_every(name, seconds[, action[, value]])`.
- `add_at(name, year, month, day, hour, minute, second[, action[, value]])`.
- `add_daily(name, hour, minute, second[, action[, value]])`.
- `add_weekly(name, weekday_mask, hour, minute, second[, action[, value]])`.
- `enable(name, enabled)`, `remove(name)`, `run(name)`, and `stop_alarm()`.

The action is `"alarm"` or `"run"`; a run action requires an absolute shell
script path. Combine `SUN` through `SAT` with bitwise OR for weekly schedules.

## Quick reference

Use `solaros.time`, `solaros.rtc`, `solaros.schedule` for time and scheduling.
See `man python` for runtime conventions and service availability.
