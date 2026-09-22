+++
id = "python.system"
title = "Python apps, jobs, and identity API"
section = "api"
summary = "Apps, jobs, and identity: identity, jobs, sessions, apps"
keywords = "python solaros api system identity jobs sessions apps"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python apps, jobs, and identity API

[API overview](python.md) · [Lua apps, jobs, and identity](lua.system.md)

## `solaros.identity`

Identity functions read the SolarOS user and hostname service.

- `user()`: return the configured username used by default for SSH and SCP.
- `hostname()`: return the configured hostname.
- `set_user(name)`: validate and save the username in NVS.
- `set_hostname(name)`: validate and save the hostname in NVS. Reboot before
  expecting an already initialized Wi-Fi interface to advertise the new name.
- `format()`: return `user@hostname`.

Existing `/.solar/user` and `/.solar/hostname` files are imported once when
their corresponding NVS keys are absent.

Example:

```python
import solaros

print(solaros.identity.format())
```

## `solaros.jobs`

Job functions control SolarOS background jobs.

- `list()`: return all jobs.
- `count()`: return number of jobs.
- `status(name)`: return one job status.
- `start(name[, args])`: start a job; `args` is a list or tuple of strings.
- `stop(name)`: stop a job.

Status dictionaries include `tick_interval_ms`, `tick_deadline_ms`,
`tick_last_us`, `tick_max_us`, and `tick_deadline_misses` in addition to the
job state and tick count. `worker_stack_bytes` is the declared launch-admission
requirement and `worker_stack_external` identifies its memory region. These
fields expose the effective cooperative scheduling policy, memory admission,
and measured handler execution time.

Example:

```python
import solaros

solaros.jobs.start("ntp-sync", ["60", "pool.ntp.org"])
print(solaros.jobs.status("ntp-sync"))
solaros.jobs.stop("ntp-sync")
```

## `solaros.sessions`

Session functions create and close foreground shell/app sessions.

- `create_shell(port[, term[, cols, rows[, charset]]])`: create a port shell session and return its numeric session id.
- `create_shell(port, term="auto", cols=80, rows=24, charset="utf8")`: keyword form for the same call. Use `charset="ascii"` for legacy terminals.
- `close(session_id)`: close a display/app session or stop a port shell session;
  closing the final interactive shell is refused.

Manual port shell sessions created from scripts do not run `/.shell/startup`.

Example:

```python
import solaros

try:
    solaros.jobs.stop("slip")
except OSError:
    pass

sid = solaros.sessions.create_shell(
    "uart0", term="ansi", cols=80, rows=25, charset="ascii"
)
# later:
solaros.sessions.close(sid)
solaros.jobs.start("slip", ["uart0", "115200"])
```

## `solaros.apps`

Application functions inspect the foreground app registry and hand control to
another app.

- `list(include_playground=True)`: return built-in apps and installed Playground
  apps. Playground apps are included by default; pass `False` for native apps
  only. Entries include `name`, `id`, `title`, `summary`, `kind`, and `runtime`.
  Playground launch names use the stable `playground:<id>` form.
- `find(name)`: return one native app dictionary or `None`.
- `launch(name, args=[])`: replace the current script app with a native or
  installed Playground app. `args` is a list or tuple of strings and does not
  include the app name. A successful handoff does not return to the script.
- `can_open(path_or_url)`: return whether a registered app handles the local
  file extension or an HTTP(S) URL.
- `open(path_or_url)`: replace the current script app with the registered
  handler. A successful handoff does not return.

Example:

```python
import solaros

for app in solaros.apps.list():
    print(app["name"], app["kind"], "-", app["summary"])

if solaros.apps.can_open("/books/guide.epub"):
    solaros.apps.open("/books/guide.epub")
```

`open()` selects Web for `http://` and `https://` URLs and uses the native file
association registry for local paths. For example, EPUB and Markdown reading
targets select Reader when it is part of the current firmware. Unknown schemes
and file types are not opened implicitly.

## Longer Example: Status Snapshot

```python
import solaros

solaros.write("SolarOS {}\n".format(solaros.version()))
solaros.write("{}\n".format(solaros.identity.format()))
solaros.write("uptime {}\n".format(solaros.time.uptime()))

battery = solaros.battery.status()
solaros.write("battery {}% {} mV\n".format(battery["percent"], battery["voltage_mv"]))

env = solaros.sensors.environment()
solaros.write("env {:.1f} C {:.1f}%\n".format(env["temperature_c"], env["humidity_percent"]))

wifi = solaros.wifi.status()
solaros.write("wifi {} {}\n".format(wifi["state"], wifi["ip"]))
```

## Quick reference

Use `solaros.identity`, `solaros.jobs`, `solaros.sessions`, `solaros.apps` for apps, jobs, and identity.
See `man python` for runtime conventions and service availability.
