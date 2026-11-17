+++
id = "lua.time"
title = "Lua time and scheduling API"
section = "api"
summary = "Time and scheduling: time, rtc, schedule"
keywords = "lua solaros api time time rtc schedule"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua time and scheduling API

[API overview](lua.md) · [Python time and scheduling](python.time.md)

## `solaros.time`

- `solaros.time`: `uptime_ms`, `sleep_ms`, `uptime`, `datetime`, `utc_datetime`, `set_datetime`, `set_utc_datetime`, `utc_to_local`, `local_to_utc`, `is_valid`, `timezone`, `set_timezone`, `ntp_sync`. `sleep_ms` is cancellation-aware and accepts delays up to one hour.
- `ntp_sync([server[, timeout_ms]])` returns `{utc = ..., ["local"] = ...}`. `server` defaults to `pool.ntp.org`, `timeout_ms` to `40000`. With less than 40000, a sync whose first packet is lost fails before lwIP's retry.

## `solaros.rtc`

- `solaros.rtc`: `status`, `set_alarm`, `clear_alarm`, `set_timer`, `clear_timer`, `pending`, and `ack`; constants `INTERRUPT_ALARM` and `INTERRUPT_TIMER`. Direct slots are leased to Lua and released when the runtime exits.

## `solaros.schedule`

- `solaros.schedule`: `list`, `add_in`, `add_every`, `add_at`, `add_daily`, `add_weekly`, `enable`, `remove`, `run`, and `stop_alarm`; weekday bit constants `SUN` through `SAT`. Actions are `"alarm"` or `"run"`, with an absolute shell-script path for `"run"`.

## Quick reference

Use `solaros.time`, `solaros.rtc`, `solaros.schedule` for time and scheduling.
See `man lua` for runtime conventions and service availability.
