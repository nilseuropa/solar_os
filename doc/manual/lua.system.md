+++
id = "lua.system"
title = "Lua apps, jobs, and identity API"
section = "api"
summary = "Apps, jobs, and identity: identity, jobs, sessions, apps"
keywords = "lua solaros api system identity jobs sessions apps"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua apps, jobs, and identity API

[API overview](lua.md) · [Python apps, jobs, and identity](python.system.md)

## `solaros.identity`

- `solaros.identity`: `user`, `hostname`, `set_user`, `set_hostname`, `format`

## `solaros.jobs`

- `solaros.jobs`: `list`, `count`, `status`, `start`, `stop`

## `solaros.sessions`

- `solaros.sessions`: `create_shell`, `close`

## `solaros.apps`

- `solaros.apps`: `list`, `find`, `launch`, `open`, `can_open`

`list([include_playground])` includes installed Playground apps by default and
returns `name`, `id`, `title`, `summary`, `kind`, and `runtime`. Playground
launch names use `playground:<id>`; pass `false` to list only native apps.

`launch(name[, args])` and `open(path_or_url)` replace the current script app,
so a successful handoff does not return. `args` is an array of strings without
the app name. `can_open(path_or_url)` reports support for local file extensions
and HTTP(S) URLs. `find(name)` continues to inspect the native registry.

## Identity

`solaros.identity.user()` and `hostname()` return the NVS-backed device
identity. `set_user(name)` and `set_hostname(name)` validate and persist new
values. Reboot before expecting an already initialized Wi-Fi interface to
advertise a changed hostname.

SSH and SCP use the identity user as their default remote username when
`user@host` is not supplied.

Existing `/.solar/user` and `/.solar/hostname` files are imported once when
their corresponding NVS keys are absent.

## Jobs

`solaros.jobs.list()` and `solaros.jobs.status(name)` return the effective
`tick_interval_ms` and `tick_deadline_ms` plus `tick_last_us`, `tick_max_us`,
and `tick_deadline_misses` runtime telemetry. `worker_stack_bytes` is the
declared launch-admission requirement and `worker_stack_external` identifies
its memory region. Job control is available through `start(name[, args])` and
`stop(name)`.

## Sessions

`solaros.sessions` creates manual port shell sessions and closes sessions by id.
Script-created port shells do not run `/.shell/startup`.

- `create_shell(port[, term[, cols, rows[, charset]]])`: create a port shell and return its numeric session id.
- `create_shell(port, {term="auto", cols=80, rows=24, charset="utf8"})`: table-options form for the same call. Use `charset="ascii"` for legacy terminals.
- `close(session_id)`: close a display/app session or stop a port shell session;
  closing the final interactive shell is refused.

Example:

```lua
local solaros = require("solaros")

pcall(function()
    solaros.jobs.stop("slip")
end)

local sid = solaros.sessions.create_shell(
    "uart0", {term = "ansi", cols = 80, rows = 25, charset = "ascii"}
)
-- later:
solaros.sessions.close(sid)
solaros.jobs.start("slip", {"uart0", "115200"})
```

## Quick reference

Use `solaros.identity`, `solaros.jobs`, `solaros.sessions`, `solaros.apps` for apps, jobs, and identity.
See `man lua` for runtime conventions and service availability.
