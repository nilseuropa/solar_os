+++
id = "python"
title = "Python API reference"
section = "api"
summary = "Complete MicroPython service API, conventions, and examples"
aliases = ["micropython", "python.api"]
keywords = "python micropython solaros api storage wifi gpio buses gfx tui examples"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# SolarOS Python API

SolarOS embeds MicroPython as the `python` foreground application. It can run an interactive REPL or execute `.py` and `.mpy` files from storage.

```text
python
python /apps/demo.py arg1 arg2
```

Scripts receive their arguments through `sys.argv`. Script output is drawn in the SolarOS terminal. The active shell's app-exit key exits the REPL or requests `KeyboardInterrupt` while code is running.

The native module is called `solaros`:

```python
import solaros

solaros.write("SolarOS " + solaros.version() + "\n")
```

## Conventions

Most mutating functions return `None` on success and raise `OSError("ESP_ERR_...")` on service failure. Query functions return strings, integers, booleans, dictionaries, or lists.

SolarOS uses MicroPython's size-conscious `EXTRA` language profile. This adds
common language features such as f-strings, sets, properties, descriptors,
`enumerate()`, `filter()`, `reversed()`, `memoryview`, and `frozenset`. The
importable runtime modules are `array`, `binascii`, `cmath`, `collections`,
`errno`, `gc`, `hashlib`, `io`, `json`, `math`, `micropython`, `random`,
`struct`, and `sys`.

`input()`, `execfile()`, and upstream `extmod` modules outside this selected
set remain disabled. Use the typed `solaros` service APIs instead.

The selected modules include `json.loads()` and `json.dumps()`, hexadecimal and
Base64 conversions in `binascii`, SHA-256 in `hashlib`, and the usual
non-cryptographic `random` helpers. SolarOS seeds `random` from the ESP32
hardware random source when the module is first imported. Use `hashlib` for
hashing and an appropriate SolarOS security service, not `random`, for
security-sensitive values.

Functions that accept file paths use SolarOS shell-style paths. `/` means the default storage mount; internally this resolves to the active storage mount point.

## Files and imports

`open(path[, mode])` opens a file through SolarOS storage. Text mode is the
default; add `b` for bytes. The supported base modes are `r`, `w`, `a`, and
exclusive-create `x`, and each can use `+` for updating. File objects support
`read`, `readinto`, `readline`, `readlines`, `write`, `seek`, `tell`, `flush`,
and `close`, and can be used as context managers.

```python
with open("/notes/example.txt", "w") as output:
    output.write("hello from Python\n")

with open("/notes/example.txt") as source:
    print(source.read())
```

Paths use the same preferred-storage and explicit-mount rules as
`solaros.storage`. They cannot bypass SolarOS mounts to reach an unrelated
host filesystem. Long reads and writes remain responsive to app cancellation.

External imports support `.py`, `.mpy`, and package directories containing
`__init__.py` or `__init__.mpy`. A file-run starts its search in the script's
directory and then searches the other entries in `sys.path`. The REPL and
source-runner start relative imports at the preferred storage root.

```text
/apps/weather/main.py
/apps/weather/sensors.py
/apps/weather/display/__init__.py
```

From `main.py`, both `import sensors` and `import display` resolve beside the
script. Imported files pass through the same SolarOS path resolver as
`open()`.

The Python runtime package requires PSRAM. Hardware and network helpers are
added only when the board/flavor includes their service package. For example,
an ODROID-GO full build includes Python with `solaros.spi` and
`solaros.onewire`, while omitting `solaros.adc` and `solaros.i2c` because those
service packages are not available on that board.

Optional API groups follow these package gates:

- `service.wifi`: top-level `wifi_status` and `solaros.wifi`
- `network.mqtt`: `solaros.mqtt`
- `network.http-client`: `solaros.http`
- `network.ftp`: `solaros.ftp`
- `network.base`: `solaros.net`
- `network.ssh`: `solaros.ssh_keys`
- `service.ble`: `solaros.ble`
- `service.hid`: `solaros.hid`
- `service.gpio`: `solaros.gpio` and `solaros.led`
- `service.onewire`: `solaros.onewire`
- `service.messaging`: `solaros.contacts` and `solaros.messages`
- `service.adc`, `service.pwm`, `service.i2c`, `service.spi`, and
  `service.uart`: their matching submodules
- `service.audio`, `service.synth`, `service.battery`, and `service.sensors`:
  their matching helpers and submodules
- `service.dsp`: `solaros.dsp` fixed-point block operations and caller-owned
  FIR, decimator, and FFT processors

```python
print(solaros.storage.resolve("/.shell/history"))
```

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

## Top-Level Helpers

- `solaros.write(text)`: write text to the SolarOS terminal.
- `solaros.version()`: return the SolarOS firmware version string.
- `solaros.should_exit()`: return `True` when the app is being asked to stop.
- `solaros.tick_interval([ms])`: get or set the foreground event-pump interval in milliseconds. Pass `0` to restore the 25 ms default.
- `solaros.battery_status()`: shortcut for `solaros.battery.status()` when battery support is compiled.
- `solaros.wifi_status()`: compact Wi-Fi status shortcut when Wi-Fi support is compiled.
- `solaros.environment()`: shortcut for `solaros.sensors.environment()` when environmental sensor support is compiled.

For example, `solaros.tick_interval(5)` lets a foreground Python app drain
terminal, TUI, and graphics events at a best-effort 5 ms cadence. It does not
schedule or preempt Python code, and it is not a hard-real-time timer. The
setting lasts for the current foreground Python app only; headless script jobs
cannot change it.

## `solaros.contacts` and `solaros.messages`

Provider-neutral messaging builds expose:

- `solaros.contacts.list()`: bounded contact summaries.
- `solaros.contacts.get(contact_id)`: one contact with endpoint IDs, or `None`.
- `solaros.messages.conversations()`: bounded conversation summaries.
- `solaros.messages.list(conversation_id)`: retained messages.
- `solaros.messages.send(conversation_id, body, allow_untrusted=False)`: queue
  a message and return its stable hexadecimal ID.
- `solaros.messages.mark_read(conversation_id)`: mark linked message state read.
- `solaros.messages.cancel(message_id)`: cancel a queued message using the
  hexadecimal string returned by `send()` or `list()`.

Scripts cannot read credentials or endpoint secret material. Blocked direct
endpoints are rejected, and discovered endpoints require
`allow_untrusted=True` for that one send.

## `solaros.storage`

Storage functions expose SD mount and filesystem service operations.

- `status()`: return a human-readable status string for the preferred mounted
  persistent storage (SD when mounted, otherwise internal flash).
- `is_mounted()`: return whether the default storage volume is mounted.
- `mount()`: mount the default storage volume.
- `unmount()`: unmount the default storage volume.
- `mount_point()`: return the preferred mounted persistent-storage path (SD
  when mounted, otherwise internal flash).
- `usage([path])`: return disk usage for the default volume or the volume containing `path`.
- `resolve(path)`: return the internal resolved path.
- `read_file(path[, max_bytes])`: return up to `max_bytes` bytes from a regular
  file. The default is 4096 and the maximum is 65536.
- `rescan()`: rescan SD block devices and partitions.
- `blocks()`: return a list of block device and partition dictionaries.
- `block_count()`: return the number of known blocks.
- `block(index)`: return one block dictionary.
- `usage_for_block(index)`: return usage for one mounted block.
- `mkdir(path)`: create a directory.
- `rmdir(path)`: remove an empty directory.
- `remove(path)`: remove a file.
- `rename(old_path, new_path)`: rename or move a file or directory.
- `copy(source_path, dest_path)`: copy a file.
- `mount_volume(name[, mount_point])`: mount a named block or partition.
- `unmount_volume(target)`: unmount by volume name or mount point.

Example:

```python
import solaros

if not solaros.storage.is_mounted():
    solaros.storage.mount()

print(solaros.storage.usage("/"))
print(solaros.storage.read_file("/notes/example.txt", 512))
for block in solaros.storage.blocks():
    print(block["name"], block["type"], block["mounted"], block["mount_point"])
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
- `ntp_sync([server[, timeout_ms]])`: sync wall-clock time from NTP and return `{"utc": ..., "local": ...}`.

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

## `solaros.battery`

Available when the firmware includes the battery service.

- `status()`: return battery status with `voltage_mv`, `percent`, `percent_estimated`, `adc_calibrated`, and `external_power`.

Example:

```python
import solaros

battery = solaros.battery.status()
print("{} mV, {}%".format(battery["voltage_mv"], battery["percent"]))
```

## `solaros.sensors`

Available when the firmware includes the environmental sensor service.

- `environment()`: return `temperature_c` and `humidity_percent`.

Example:

```python
import solaros

env = solaros.sensors.environment()
print("{:.1f} C {:.1f}%".format(env["temperature_c"], env["humidity_percent"]))
```

## `solaros.wifi`

Wi-Fi functions expose station, SoftAP, scan, NAT, and L2 IPv4 repeater controls.

- `status()`: return detailed Wi-Fi status.
- `status_text()`: return the same compact status text used by the shell.
- `start()`: start Wi-Fi and reconnect saved station config if present.
- `stop()`: stop Wi-Fi.
- `connect(ssid[, password])`: connect to a station network and save it.
- `connect_saved()`: connect using remembered station profiles.
- `disconnect()`: disconnect station mode.
- `forget()`: remove the active or preferred station profile.
- `forget_ssid(ssid)`: remove one remembered station profile.
- `forget_all()`: remove all remembered station profiles.
- `known()`: return remembered station profiles as dictionaries with `ssid` and `preferred`.
- `scan()`: return visible APs as dictionaries with `ssid`, `auth`, `rssi`, `channel`, and `hidden`.
- `ap_start([ssid[, password[, auth]]])`: start SoftAP, reusing saved AP config when no arguments are supplied.
- `ap_stop()`: stop SoftAP.
- `nat(enabled)`: persistently enable or disable APSTA NAT.
- `repeater_start()`: connect the preferred remembered upstream when needed, repeat its saved SSID and password, and bridge upstream DHCP plus IPv4/ARP traffic without NAT.
- `repeater_stop()`: stop L2 forwarding and the SoftAP while retaining the upstream station.

Example:

```python
import solaros

solaros.wifi.start()
print(solaros.wifi.status())

for ap in solaros.wifi.scan():
    print(ap["rssi"], ap["auth"], ap["ssid"])
```

## `solaros.mqtt`

MQTT functions expose the shared SolarOS MQTT service. Broker URL and credentials are stored in NVS, so they work without an SD card.

- `status()`: return MQTT status, saved broker URL, client ID, auth flags, counters, queued message count, and last error.
- `connect([url[, username[, password]]])`: connect to `mqtt://...` or `mqtts://...`; supplied values are saved in NVS. With no arguments, reconnect using saved settings.
- `disconnect()`: stop the MQTT client.
- `publish(topic, payload[, qos[, retain]])`: publish bytes or text and return the message ID.
- `subscribe(topic[, qos])`: subscribe and return the message ID.
- `read([timeout_ms])`: return the next queued message dictionary, or `None` on timeout. Message payloads are returned as bytes.

Example:

```python
import solaros

solaros.mqtt.connect("mqtts://broker.example.com:8883", "user", "secret")
solaros.mqtt.publish("solaros/status", b"online", 0, False)
solaros.mqtt.subscribe("solaros/inbox/#")

while not solaros.should_exit():
    msg = solaros.mqtt.read(1000)
    if msg:
        print(msg["topic"], msg["payload"])
```

## `solaros.http`

`solaros.http` provides bounded synchronous HTTP and HTTPS requests through the
shared SolarOS HTTP client. It is present when `network.http-client` is
compiled. HTTPS uses the firmware certificate bundle; no socket, TLS, or
upstream MicroPython networking module is exposed.

- `request(method, url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `get(url[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]])`
- `head(url[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]])`
- `post(url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `put(url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `patch(url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `delete(url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `session_open(origin)`
- `session_request(handle, method, url[, body[, headers[, timeout_ms[, max_bytes]]]])`
- `session_close(handle)`
- `session_close_all()`
- `stream_open(method, url[, body[, headers[, timeout_ms[, follow_redirects]]]])`
- `stream_read(handle[, timeout_ms])`
- `stream_close(handle)`
- `stream_close_all()`

Methods are case-insensitive in `request()`. Request bodies accept text or any
readable buffer. URLs must use `http://` or `https://`. Headers are a dictionary
of up to 16 string pairs and 8192 bytes total; names and values cannot contain
line breaks. The defaults are a 10000 ms end-to-end timeout, a 65536-byte
response-body limit, and redirect following. `max_bytes` accepts 0 through
262144; zero collects metadata without retaining a body.

The result is a dictionary containing `status_code`, binary `body`, `headers`,
`content_length`, `bytes_received`, `duration_ms`, `truncated`, and
`headers_truncated`. Header names retain the server's spelling and duplicate
names use the last received value. `content_length` is `-1` when the server did
not supply it. When a body exceeds `max_bytes`, SolarOS stops that response,
returns the retained prefix, and sets `truncated=True`.

HTTP error statuses such as 404 and 500 are normal results. Invalid requests,
allocation failures, cancellation, deadlines, DNS failures, and transport
errors raise `OSError("ESP_ERR_...")`. Exiting the Python app cancels an active
request.

`session_open()` retains one same-origin HTTP/TLS client and returns an
interpreter-owned opaque handle. `origin` contains only the scheme and
authority, for example `https://example.com`; credentials, paths, queries, and
fragments are rejected. `session_request()` has the same response and body
limits as `request()`, but redirects are always disabled and the full request
URL must match the opened origin. Per-request headers are removed before every
request. A stale retained connection is retried once only for GET or HEAD and
only before response headers or body data arrive. Writes are never retried.
Each runtime can retain two sessions, with four sessions globally. Close
handles in `finally`; all retained clients also close at interpreter teardown.

`stream_open()` starts a native worker and returns an interpreter-owned opaque
handle. Unlike `request()`, it has no end-to-end deadline: `timeout_ms` bounds
each connect, header, write, or body-read operation and accepts 0 through 60000;
zero selects the 10000 ms service default.
`stream_read()` returns `None` when its wait expires. Otherwise it returns the
next ordered event dictionary:

- `header`: `status_code`, `name`, `value`, and `truncated`
- `response`: `status_code` and `content_length`
- `data`: `status_code` and up to 1024 binary bytes in `data`
- `complete` or `error`: final status, content length, received byte count,
  duration, cancellation/deadline flags, and ESP error details

Each runtime can open two streams, with four streams globally. Each stream has
an eight-event queue. If a script does not drain it, SolarOS terminates the
stream with `ESP_ERR_NO_MEM`; it never silently drops body bytes. Handles close
automatically at interpreter teardown. Always close them explicitly in
`finally` so a completed stream releases its queue and handle immediately.

The data boundary is a transport chunk, not an application record. For SSE,
retain an incomplete line across `data` events and dispatch a message only at
the blank line that terminates the SSE record.

```python
import solaros

response = solaros.http.get("https://example.com/")
print(response["status_code"], len(response["body"]))

response = solaros.http.post(
    "https://example.com/api",
    b'{"state":"online"}',
    {"Content-Type": "application/json"},
)
print(response["status_code"], response["body"])
```

```python
handle = solaros.http.session_open("https://example.com")
try:
    first = solaros.http.session_request(handle, "GET", "https://example.com/a")
    second = solaros.http.session_request(handle, "GET", "https://example.com/b")
finally:
    solaros.http.session_close(handle)
```

```python
handle = solaros.http.stream_open(
    "GET",
    "https://example.com/events",
    None,
    {"Accept": "text/event-stream"},
)
pending = b""
try:
    while not solaros.should_exit():
        event = solaros.http.stream_read(handle, 250)
        if event and event["type"] == "data":
            pending += event["data"]
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                print(line.rstrip(b"\r"))
        elif event and event["type"] in ("complete", "error"):
            break
finally:
    solaros.http.stream_close(handle)
```

## `solaros.gpio`

GPIO functions expose only runtime-safe expansion pins. Use `solaros.gpio.pins()`
to inspect the active board. On the Waveshare ESP32-S3-RLCD-4.2 this is GPIO1,
GPIO2, GPIO3, GPIO17, plus releasable GPIO43/GPIO44 while `uart0` is detached. On the ESP32-S3-DevKitC-1-N16R8 this is GPIO1,
GPIO2, GPIO4, GPIO5, GPIO6, GPIO7, GPIO10, GPIO14, GPIO15, GPIO16, GPIO17,
GPIO18, GPIO21, GPIO39, GPIO40, GPIO41, GPIO42, and GPIO47. On ODROID-GO this
is GPIO4 and GPIO15. On the Elecrow CrowPanel ESP32-S3 4.2-inch E-paper this is
GPIO8, GPIO9, GPIO14, GPIO15, GPIO16, GPIO17, GPIO18, GPIO19, GPIO20, GPIO21,
and GPIO38.

- Constants: `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`.
- `pins()`: return board GPIO dictionaries with `pin`, `expansion`, `allowed`,
  `available`, `claimed`, `owner`, `policy`, `role`, `configured`, `mode`,
  `pull`, `level`, and `level_valid`. Pin policy is `free`, `releasable`, or
  `fixed`; releasable pins report `allowed=True` but become available only when
  their board bus is detached.
- `allowed(pin)`: return whether a pin can be controlled by runtime apps.
- `mode(pin)`: return one pin dictionary.
- `mode(pin, mode[, pull])`: configure an allowed pin. `mode` may be `INPUT`, `OUTPUT`, `"in"`, `"input"`, `"out"`, or `"output"`.
- `configure(pin, mode[, pull])`: alias for `mode(pin, mode[, pull])`.
- `read(pin)`: read an allowed pin and return `0` or `1`.
- `write(pin, value)`: set an allowed pin low or high. If needed, the pin is configured as output first.
- `release(pin)`: reset the pin and release its direct-GPIO claim.

Example:

```python
import solaros

for pin in solaros.gpio.pins():
    print(pin)

solaros.gpio.mode(17, solaros.gpio.INPUT, solaros.gpio.PULL_UP)
print("GPIO17", solaros.gpio.read(17))

solaros.gpio.write(1, 1)
```

## `solaros.onewire`

OneWire functions operate on runtime-safe expansion GPIOs when the OneWire
service is included in the active flavor. Use `solaros.buses.onewire_*` for a
registered named bus. Transfers reset the bus before writing and reading, and
are limited to 64 bytes in each direction.

- `allowed(pin)`: return whether the pin is available for OneWire operations.
- `reset(pin)`: reset the bus and return whether a presence pulse was detected.
- `scan(pin)`: return device dictionaries containing a 16-digit hexadecimal `address` and numeric `family` code.
- `xfer(pin, read_len[, data])`: reset the bus, write a bytes-like object, then read and return `read_len` bytes. Either `read_len` or `data` must be non-empty.

Example:

```python
import solaros

for device in solaros.onewire.scan(17):
    print(device["address"], device["family"])

# Skip ROM, issue a command, and read two response bytes.
response = solaros.onewire.xfer(17, 2, b"\xcc\x44")
print(response)
```

## `solaros.led`

Status LED functions control a built-in board status LED when the board has one.

- `status()`: return whether the status LED is currently on.
- `set(on)`: set the status LED and return the resulting boolean state.
- `on()`: turn the status LED on and return `True`.
- `off()`: turn the status LED off and return `False`.
- `toggle()`: toggle the status LED and return the resulting boolean state.

Example:

```python
import solaros

solaros.led.toggle()
```

## `solaros.adc`

ADC functions expose analog reads on runtime-safe expansion pins that are ADC
capable. Some runtime GPIOs are digital-only; check `adc_capable` from
`solaros.adc.pins()` before reading.

- `pins()`: return dictionaries with `pin`, `allowed`, `adc_capable`, `unit`, and `channel`.
- `read(pin)`: return `pin`, `raw`, `voltage_mv`, `unit`, `channel`, and `calibrated`.

Example:

```python
import solaros

print(solaros.adc.pins())
print(solaros.adc.read(1))
```

## `solaros.controls`

Continuous controls are named normalized values. Python can configure them,
inspect their runtime counters, and supply manual values without knowing
whether their targets are native app parameters or MIDI CC messages.

- `list()`: return complete control configuration, normalized value, source
  value, generation, sample/update counters, read errors, and last error.
- `get(name)`: return the current normalized value from `0.0` through `1.0`.
- `set(name, value)`: set a manual control to a normalized value from `0.0`
  through `1.0`.
- `create(name[, source, input_min, input_max, smoothing_ms, deadband,
  inverted])`: create a manual or scalar-stream control and return its
  dictionary. Omit `source` or pass `None` for a manual control.
- `delete(name)` and `clear()`: remove one or all controls. `clear()` returns
  the number removed.
- `bindings()`: return parameter/MIDI targets and their pickup, application,
  and error state.
- `bind_parameter(name, path[, pickup])` and
  `bind_midi(name, channel, controller)`: add a target and return its numeric
  binding ID.
- `unbind(name)`: remove all targets for a control and return the count.

Create and bind a manual control directly:

```python
import solaros

solaros.controls.create("expression")
solaros.controls.bind_parameter(
    "expression", "synth.filter.resonance", False
)
solaros.jobs.start("controls")
solaros.controls.set("expression", 0.5)
print(solaros.controls.get("expression"))
```

## `solaros.parameters`

Native applications publish parameters only while they are active.

- `list()`: return path, owner, name, label, unit, range, step, curve, current
  value, readability, and error fields for every published parameter.
- `get(path)`: read a native-unit value.
- `set(path, value)`: set a native-unit value and return the authoritative
  value after range/step handling.

## `solaros.midi`

The MIDI job must own a running MIDI bus before scripts transmit or receive.
Create the bus with `solaros.buses.create_midi()` and start it with
`solaros.jobs.start("midi", ["midi0"])`.

- `status()`: return running state, bus name, RX/TX byte and message counts,
  parser/subscriber/queue drops, last error, CC-stream count, and whether this
  interpreter has an active receive subscription.
- `send(status[, data1, data2])`: validate and queue one raw MIDI message. The
  argument count must match the status byte.
- `note_on(channel, note[, velocity])`, `note_off(channel, note[, velocity])`,
  `cc(channel, controller, value)`, and `program(channel, program)`: queue
  channel messages. Channels are `1..16`; MIDI data is `0..127`.
- `read([timeout_ms])` or `receive([timeout_ms])`: lazily create a non-consuming
  interpreter subscription and return the next message dictionary, or `None`.
  Timeout is bounded to 60 seconds and is cancellation-aware.
- `close()`: release the receive subscription early. Interpreter shutdown also
  releases it automatically.
- `streams()`, `stream_add(channel, controller)`,
  `stream_remove(channel, controller)`, and `stream_clear()`: manage bounded
  incoming CC scalar streams.

Received and transmitted message dictionaries contain `status`, `length`,
`type`, optional `channel`, and the applicable `data1`/`data2` bytes.

```python
solaros.buses.create_midi("midi0", {"tx": 2, "rx": 3})
solaros.jobs.start("midi", ["midi0"])
solaros.midi.note_on(1, 60, 100)
message = solaros.midi.read(1000)
```

## `solaros.osc`

OSC bindings configure the native `osc` job; start and stop that worker through
`solaros.jobs`. The scripting API does not replace its bounded UDP transport,
peer filtering, or rate limiting.

- `bindings()`: return complete source configuration and runtime availability,
  value, timing, send, and error telemetry.
- `bind_stream(name, source, address[, rate_hz, delta, send_always])`: publish
  a scalar stream and return its binding ID.
- `bind_event(name, source, address[, edge, rate_hz])`: publish sampled event
  edges. `edge` is `"rising"`, `"falling"`, or `"both"`.
- `bind_control(name, control, address[, rate_hz, send_always])`: publish a
  normalized named control and return its binding ID.
- `unbind(name)` and `clear()`: remove one or all bindings. `clear()` returns
  the number removed.
- `encode_float(address, value)` and `encode_int(address, value)`: return a
  bounded OSC message as `bytes`, suitable for `solaros.net.udp_send()`.
- `dispatch(packet)`: validate a message or immediate bundle and apply its
  native parameter routes; return message, applied, unknown, and rejected
  counts.
- `limits()`: return packet, address, binding, bundle/update, and rate limits.

```python
solaros.osc.bind_stream(
    "ambient", "temperature", "/room/temperature", 2.0, 0.1
)
solaros.jobs.start(
    "osc", ["listen=9000", "target=192.168.1.50:9001"]
)
```

## `solaros.pwm`

PWM functions expose LEDC PWM output on runtime-safe expansion pins. Active PWM outputs share one LEDC timer, so changing the frequency changes the frequency for all active PWM outputs.

- Constants: `FREQ_MIN`, `FREQ_MAX`.
- `status()`: return dictionaries with `pin`, `allowed`, `active`, `channel`, `freq_hz`, and `duty_percent`.
- `set(pin, freq_hz, duty_percent)`: start or update PWM on a pin. Duty is `0..100`.
- `off(pin)`: stop PWM on a pin.

Example:

```python
import solaros

solaros.pwm.set(1, 1000, 50)
print(solaros.pwm.status())
solaros.pwm.off(1)
```

## `solaros.buses`

The named-bus API discovers board-defined and runtime-created buses. It is
available when the resource service is compiled, independently of the legacy
single-board-bus `solaros.spi` module.

- Constants: `MODE0` through `MODE3`, `SPI2_HOST`, `SPI3_HOST`,
  `DEFAULT_SPEED`, and `MAX_SPEED`.
- `list()`: return all named bus dictionaries.
- `get(name)`: return one named bus dictionary or raise `OSError` when absent.
- `create_i2c(name, config)`: create a runtime I2C bus and return its dictionary.
- `create_onewire(name, config)`: create a runtime 1-Wire bus and return its dictionary.
- `create_ps2(name, config)`: create an exclusive PS/2 bus from `clock` and `data` pins.
- `create_spi(name, config)`: create a runtime SPI bus and return its dictionary.
- `create_uart(name, config)`: create a lazy runtime UART bus and return its dictionary.
- `create_midi(name, config)`: create an exclusive MIDI bus and automatically select its UART backend.
- `attach(name)`: attach a named detachable bus and reserve its endpoint and pins.
- `detach(name)`: detach an idle named bus without deleting its descriptor.
- `remove(name)`: remove an idle runtime bus. Board-defined or leased buses
  cannot be removed.
- `i2c_probe(bus, address)`: probe an address on a named I2C bus.
- `i2c_scan(bus)`: return detected addresses on a named I2C bus.
- `i2c_read_reg(bus, address, reg, length)`: read bytes from an 8-bit register.
- `i2c_write_reg(bus, address, reg, data)`: write bytes to an 8-bit register.
- `onewire_reset(bus)`: reset a named 1-Wire bus and return device presence.
- `onewire_scan(bus)`: return ROM-address dictionaries found on a named bus.
- `onewire_xfer(bus, read_length[, data])`: reset, write, and read a named bus.
- `uart_write(bus, data)`: write bytes through a named UART and return the number written.
- `uart_read(bus[, length[, timeout_ms]])`: read bytes from a named UART.
- `spi_xfer(bus, cs, data[, mode[, speed_hz]])`: perform a full-duplex named-bus
  transfer and return received bytes.
- `spi_read(bus, cs, length[, fill[, mode[, speed_hz]]])`: clock in bytes using
  the optional fill byte.
- `spi_write(bus, cs, data[, mode[, speed_hz]])`: write bytes and return the
  number written.

Bus dictionaries contain `id`, `name`, `protocol`, `origin`, `sharing`,
`attached`, `detachable`, `ready`, and `lease_count`, plus protocol-specific pins and configuration. SPI
buses include `host`, `sclk_pin`, `miso_pin`, `mosi_pin`,
`max_transfer_size`, and `cs` slot dictionaries. I2C buses include `port`,
`sda_pin`, `scl_pin`, and `speed_hz`. UART and MIDI buses include `port`, `tx_pin`,
`rx_pin`, and `baud_rate`.

Named I2C operations are present when both the resource and I2C services are
compiled. They take and release a shared bus lease automatically. The legacy
`solaros.i2c` module remains an `i2c0` shortcut.

Named OneWire operations are present when both the resource and OneWire
services are compiled. They take and release an exclusive bus lease
automatically. OneWire bus dictionaries include `pin`; the legacy
`solaros.onewire` module continues to accept a direct runtime-safe GPIO.

`create_i2c` requires `port`, `sda`, and `scl`; optional `speed_hz` defaults to
100000. `create_onewire` requires `pin`. Both validate the board runtime pin
policy and claim their signal pins until `remove(name)`.

`create_uart` requires `port`, `tx`, and `rx`; optional `baud_rate` defaults to
115200. Named UART reads and writes take an exclusive lease automatically.
Runtime descriptors are detachable and removable. Board descriptors whose
signal pins are marked releasable are detachable but never removable;
fixed-pin board descriptors reject detach. Attached buses own their hardware
endpoint and signal pins, while protocol hardware starts for the first lease.

`create_midi` requires `tx` and `rx`; optional `baud_rate` defaults to 31250.
SolarOS selects an unused board-approved UART controller. The returned `port`
is diagnostic backend information, not an input to the MIDI API.

`create_spi` accepts a configuration dictionary with required `host`, `sclk`,
`mosi`, and `cs` fields. `cs` is a list of one to four chip-select GPIOs.
Optional fields are `miso` (`None` for transmit-only) and
`max_transfer_size` (default 4096 bytes). The board validates the selected host
and all signal pins. Raw named-bus transfers take and release a temporary bus
lease automatically.

Example for a runtime-routed Waveshare SPI bus:

```python
import solaros

bus = solaros.buses.create_spi("spi1", {
    "host": solaros.buses.SPI3_HOST,
    "sclk": 1,
    "mosi": 2,
    "miso": 3,
    "cs": [17],
})
print(bus)

reply = solaros.buses.spi_xfer("spi1", "gpio17", b"\x9f\x00\x00\x00")
print(reply)

solaros.buses.remove("spi1")
```

Runtime I2C and 1-Wire examples:

```python
i2c1 = solaros.buses.create_i2c("i2c1", {
    "port": 1,
    "sda": 14,
    "scl": 15,
    "speed_hz": 100000,
})
print(solaros.buses.i2c_scan(i2c1["name"]))
solaros.buses.remove(i2c1["name"])

onewire0 = solaros.buses.create_onewire("onewire0", {"pin": 16})
print(solaros.buses.onewire_scan(onewire0["name"]))
solaros.buses.remove(onewire0["name"])

uart1 = solaros.buses.create_uart("uart1", {
    "port": 1,
    "tx": 14,
    "rx": 15,
    "baud_rate": 115200,
})
solaros.buses.uart_write(uart1["name"], b"AT\r\n")
print(solaros.buses.uart_read(uart1["name"], 64, 500))
solaros.buses.detach(uart1["name"])
solaros.buses.attach(uart1["name"])
solaros.buses.remove(uart1["name"])
```

Named I2C example:

```python
import solaros

print(solaros.buses.get("i2c0"))
print([hex(addr) for addr in solaros.buses.i2c_scan("i2c0")])
solaros.buses.i2c_probe("i2c0", 0x3c)
```

Named OneWire example for a board-defined bus:

```python
import solaros

print(solaros.buses.get("onewire0"))
print(solaros.buses.onewire_reset("onewire0"))
for device in solaros.buses.onewire_scan("onewire0"):
    print(device["address"], device["family"])
```

## `solaros.expansion`

The expansion API mirrors the `expansion` shell lifecycle when the expansion
service is compiled.

- `drivers()`: return compiled driver dictionaries with `name`, `summary`,
  `required_capabilities`, `probe_supported`, and `supported`.
- `devices()`: return active device dictionaries with `name`, `driver`,
  `origin` (`board` or `runtime`), `ready`, `autostart`, `detachable`, and
  `bindings`. Each normalized binding contains `kind`, `role`, `target`,
  `value`, and `aux`.
- `attach(driver, name, bindings)`: attach a driver using a binding dictionary.
- `detach(name)`: detach a device and release its resource claims and bus leases.

Binding dictionaries accept `spi`, `cs` (or `ce`), `i2c`, `addr`, `uart`,
`ps2`, `gpio`, `irq`, `reset` (or `rst`), `data`, `bck`, `din`, `rck`, `dc`,
`mclk`, `ws`, `dout`, `busy`, `adc`, `pwm`, `count`, `keys`, `x`, `y`, `min`,
`center`, `max`, and `deadzone`. `ps2` names an existing PS/2 bus; `x` and `y`
name scalar streams;
`keys` maps logical key names to GPIO numbers. `cs` requires `spi`, and `addr`
requires `i2c`. Unknown keys are rejected.

```python
import solaros

solaros.expansion.attach("pcd8544", "lcd0", {
    "spi": "spi0",
    "cs": 10,
    "dc": 4,
    "reset": 5,
})
print(solaros.expansion.devices())
solaros.expansion.detach("lcd0")
```

## `solaros.neopixel`

Available when the NeoPixel expansion package is compiled.

- `list()`: return attached strip dictionaries with `name`, `data_pin`, and `count`.
- `set(name, index, red, green, blue)`: update one buffered pixel.
- `fill(name, red, green, blue)`: update every buffered pixel.
- `show(name)`: transmit the buffered colors in GRB wire order.
- `clear(name)`: clear the buffer and transmit it immediately.

```python
import solaros

solaros.expansion.attach("neopixel", "pixels0", {"data": 1, "count": 8})
solaros.neopixel.fill("pixels0", 0, 0, 8)
solaros.neopixel.set("pixels0", 3, 16, 0, 0)
solaros.neopixel.show("pixels0")
```

## `solaros.i2c`

I2C functions expose `i2c0` for diagnostics and compatibility. Use
`solaros.buses.i2c_*` to select a named bus.

- `info()`: return bus speed and SDA/SCL pins.
- `probe(address)`: raise on missing device, return `None` on success.
- `scan()`: return detected addresses.
- `read_reg(address, reg, length)`: read bytes from an 8-bit register.
- `write_reg(address, reg, data)`: write bytes to an 8-bit register.

Example:

```python
import solaros

print(solaros.i2c.info())
print([hex(addr) for addr in solaros.i2c.scan()])
```

## `solaros.spi`

Available when the board and flavor include the SPI service. This compatibility
module selects `spi0` when present, otherwise the first registered named SPI
bus. On a dynamic-only board, `status()["available"]` remains `False` until a
bus is created. Chip select may be a configured CS name from `status()["cs"]`
or its configured numeric GPIO. Transfers are limited to the selected bus's
reported `max_transfer_size`; new code should address buses explicitly through
`solaros.buses.spi_*`.

- Constants: `MODE0`, `MODE1`, `MODE2`, `MODE3`, `DEFAULT_SPEED`, `MAX_SPEED`.
- `status()`: return the bus name, host, pins, speed, transfer limit, and configured CS slots.
- `xfer(cs, data[, mode[, speed_hz]])`: perform a full-duplex transfer and return the received bytes.
- `read(cs, length[, fill[, mode[, speed_hz]]])`: transmit the fill byte, default `0xff`, while reading.
- `write(cs, data[, mode[, speed_hz]])`: write bytes and return the number written.

Example:

```python
import solaros

status = solaros.spi.status()
cs = status["cs"][0]["name"]

# JEDEC ID command followed by three dummy bytes in one CS transaction.
response = solaros.spi.xfer(cs, b"\x9f\x00\x00\x00", solaros.spi.MODE0, 1_000_000)
print(response[1:])
```

## `solaros.uart`

UART functions expose the default `uart0` compatibility service. Use
`solaros.buses.uart_*` to address another named UART bus.

- `status()`: return UART name, `attached`, port, pins, baud rate, mode, `rx_buffered`, and `rx_buffered_valid`. When another owner is actively using the UART, `rx_buffered_valid` is `False` because the live RX count is not sampled.
- `baud([rate])`: get or set baud rate.
- `is_valid_baud(rate)`: return whether a baud rate is accepted.
- `mode([name])`: get or set `raw` or `line` mode.
- `write(data)`: write bytes and return bytes written.
- `read([length[, timeout_ms]])`: read bytes.

Example:

```python
import solaros

solaros.uart.baud(115200)
solaros.uart.mode("raw")
solaros.uart.write(b"AT\r\n")
print(solaros.uart.read(64, 500))
```

## `solaros.audio`

Available when the firmware includes the audio service.

Audio functions expose the microphone, speaker, and WAV service.

- `status()`: return codec/sample/pin status.
- `deinit()`: turn audio hardware off.
- `off()`: alias for `deinit()`.
- `set_volume(volume)`: set speaker volume.
- `set_mic_gain(gain_db)`: set microphone gain.
- `tone(frequency_hz, duration_ms[, volume])`: play a tone.
- `tone_async(frequency_hz, duration_ms[, volume])`: queue a tone and return its request ID.
- `cancel(request_id)`: cancel a queued or playing asynchronous tone.
- `queue_status()`: return asynchronous tone worker, queue, and result counters.
- `level(duration_ms)`: measure input level and return samples, peak, and average percent.
- `capture(frames)`: capture 1 through 4096 native input frames and return
  `(pcm, format)`. `pcm` contains interleaved little-endian signed-16 samples.
  `format` contains `sample_format`, `sample_rate`, `channels`, and
  `bits_per_sample`.
- `loopback(duration_ms[, volume])`: run microphone-to-speaker loopback.
- `wav_info(path)`: inspect a WAV file.
- `record_wav(path, duration_ms)`: record a native WAV file.
- `play_wav(path[, volume])`: play a native WAV file.

Example:

```python
import solaros

print(solaros.audio.status())
solaros.audio.tone(880, 200, 40)
sound = solaros.audio.tone_async(1175, 70)
print(solaros.audio.queue_status())
print(solaros.audio.level(500))
pcm, format = solaros.audio.capture(1024)
print(len(pcm), format)
```

## `solaros.synth`

Available when the firmware includes the synth service. The native engine has
eight voices and renders continuously without running Python in the real-time
audio callback. It uses the system's global speaker volume.

- `status()`: return ownership, both oscillator configurations, amplifier and filter envelopes, mono and glide settings, voice, sample-rate, render-deadline, and captured-PCM telemetry. The `pcm_peak` and `pcm_rms` fields come from the shared DSP service.
- `configure(waveform[, attack_ms[, decay_ms[, sustain_percent[, release_ms]]]])`: configure active voices immediately and set the defaults for future notes. Waveforms are `square`, `triangle`, `saw`, `sine`, and `noise`; envelope times are 0 through 10000 ms and sustain is 0 through 100 percent.
- `configure_oscillator2(waveform[, octave[, detune_cents[, mix_percent]]])`: configure the second oscillator. Octave is -2 through +2, detune is -100 through +100 cents, and mix is 0 through 100 percent. A zero mix is an exact oscillator-1 bypass.
- `configure_performance([mono[, glide_ms]])`: select polyphonic or monophonic last-note playback and set portamento from 0 through 2500 ms.
- `configure_filter(cutoff_hz[, resonance_percent[, envelope_amount_percent[, attack_ms[, decay_ms[, sustain_percent[, release_ms]]]]]])`: configure the resonant low-pass filter. Cutoff is 40 through 18000 Hz; percentages are 0 through 100; times are 0 through 10000 ms.
- `note_on(frequency_hz[, velocity])`: start or retrigger a note from 20 through 8000 Hz. Velocity defaults to 100 and ranges from 1 through 127.
- `note_off(frequency_hz)`: release the matching note.
- `all_notes_off()`: release all active notes through their configured release envelopes.
- `stop()`: stop immediately and release audio ownership.

The first `note_on()` claims the exclusive audio output lazily. A script also
releases that ownership automatically when it exits or is interrupted.

```python
import solaros

solaros.synth.configure("saw", 5, 80, 65, 140)
solaros.synth.configure_oscillator2("square", 0, 7, 35)
solaros.synth.configure_filter(1200, 35, 80, 5, 250, 20, 180)
solaros.synth.configure_performance(True, 80)
solaros.synth.note_on(440, 110)
solaros.synth.note_on(554, 90)
solaros.time.sleep_ms(250)
solaros.synth.all_notes_off()
solaros.time.sleep_ms(150)
solaros.synth.stop()
```

## `solaros.ble`

BLE functions expose keyboard pairing and layout controls.

- `status()`: return human-readable BLE keyboard status.
- `connected()`: return whether a keyboard is connected.
- `pair()`: start keyboard pairing.
- `forget()`: remove remembered keyboard pairing.
- `layout([name])`: get or set keyboard layout, currently `us` or `de`.
- `read([max_bytes])`: read pending decoded keyboard bytes.

Example:

```python
import solaros

print(solaros.ble.status())
print("layout", solaros.ble.layout())
```

## `solaros.hid`

`service.hid` is retained as a dormant package and is not compiled into the
standard SolarOS flavors because the TinyUSB composite stack currently costs
too much internal SRAM. On an ESP32-S3 build that explicitly enables it, USB
remains a composite device: the existing `cdc0` serial interface is accompanied
by standard keyboard, mouse, and gamepad HID reports. The API is typed; scripts
cannot replace descriptors or send arbitrary report bytes.

```python
from solaros import hid

hid.keyboard.press(hid.KEY_LEFT_CTRL, hid.KEY_C)
hid.keyboard.release_all()

hid.mouse.move(10, -4)
hid.mouse.button(hid.MOUSE_LEFT, True)
hid.mouse.button(hid.MOUSE_LEFT, False)

hid.gamepad.axis(hid.AXIS_X, -12000)
hid.gamepad.button(1, True)
hid.gamepad.hat(hid.HAT_UP)
hid.gamepad.send()
```

- `status()` returns `initialized` and `connected`.
- `keyboard.press(*keys)` and `keyboard.release(*keys)` preserve each accepted
  keyboard state transition; up to six ordinary keys plus modifiers can be
  held. `keyboard.release_all()` releases every key.
- `mouse.move(x, y)` accumulates signed deltas until transmitted.
  `mouse.button(mask, pressed)` changes one or more standard button bits.
- Gamepad setters update coalesced state. Axes use `-32768..32767`, buttons are
  numbered `1..32`, hats use `HAT_CENTERED` or one of eight directions, and
  `gamepad.send()` queues the current state.

Calls raise `OSError("ESP_ERR_INVALID_STATE")` while USB is disconnected or
HID is unavailable. SolarOS emits neutral keyboard, mouse, and gamepad reports
when the Python runtime exits, is interrupted, or is force-stopped.

## `solaros.clipboard`

The clipboard is PSRAM-backed and shared with SolarOS apps that use the clipboard service.

- `set(data)`: set clipboard bytes.
- `get()`: return clipboard bytes.
- `size()`: return clipboard size in bytes.
- `clear()`: clear the clipboard.

Example:

```python
import solaros

solaros.clipboard.set(b"hello from python")
print(solaros.clipboard.get())
```

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

## `solaros.net`

- `ping(host[, count[, timeout_ms[, interval_ms[, data_size]]]])`: ping a host and return transmit/receive statistics.
- `tcp_connect(host, port[, timeout_ms])`: open an IPv4 TCP client and return a managed handle.
- `tcp_send(handle, data[, timeout_ms])`: send the complete binary buffer.
- `tcp_receive(handle[, max_bytes[, timeout_ms]])`: receive bytes, `None` on timeout, or `b""` when the peer closes.
- `udp_open([local_port])`: open an IPv4 UDP endpoint; zero or an omitted port selects an ephemeral local port.
- `udp_send(handle, host, port, data[, timeout_ms])`: send one complete datagram.
- `udp_receive(handle[, max_bytes[, timeout_ms]])`: receive one datagram dictionary or `None` on timeout.
- `websocket_connect(url[, subprotocol[, timeout_ms]])`: connect to a `ws://` or certificate-validated `wss://` URL and return a managed handle.
- `websocket_send(handle, data[, text[, timeout_ms]])`: send one final binary frame, or one final text frame when `text` is true.
- `websocket_receive(handle[, max_bytes[, timeout_ms]])`: receive one frame dictionary or `None` on timeout.
- `close(handle)`: close one managed handle.
- `close_all()`: close every handle owned by this interpreter run.
- `limits()`: return current ownership, usage, limits, and blocking-policy fields.

The TCP, UDP, and WebSocket calls are synchronous. They do not start a worker
or invoke callbacks. The connect default is 10000 ms, the send and receive
default is 1000 ms, and each call accepts a timeout from 0 through 60000 ms.
A receive timeout is a normal `None` result. Connect, send, cancellation, DNS,
TLS, protocol, and other transport failures raise `OSError`.

Handles belong only to the current Python app or runner invocation. They cannot
be shared with Lua, another Python invocation, or a background job. Handles are
generation checked, so a closed handle does not become valid when its slot is
reused. Normal exit, an exception, cancellation, and forced interpreter cleanup
close all remaining handles before the VM is destroyed. Explicit `close()` in a
`finally` block is still recommended when the script continues after an error.

One interpreter run can hold four TCP, UDP, and WebSocket handles in total.
SolarOS allows eight of these script handles globally. A send, receive buffer,
or WebSocket frame is limited to 65536 bytes; one UDP datagram is limited to
65507 bytes. `limits()` reports these constants and the current per-run and
global handle counts. Opening past a limit raises an allocation-style
`OSError` without evicting another owner.

TCP and UDP socket waits check cancellation in slices of at most 50 ms. DNS
resolution is a platform call and checks cancellation before and after it.
WebSocket DNS, TCP/TLS setup, upgrade, and frame I/O use the supplied bounded
transport deadline and check cancellation before and after each transport
stage; cancellation can therefore take up to that stage's timeout. TCP and UDP
use one end-to-end deadline per public operation, including DNS where SolarOS
controls it. A WebSocket public call can contain multiple transport stages;
each stage is separately bounded by the supplied timeout. Receive polling,
reading, and draining share the remaining public-call deadline at the SolarOS
layer.

UDP receive dictionaries contain `data`, `address`, `port`, `truncated`, and
`datagram_bytes`. WebSocket receive dictionaries contain `data`, `type`,
`final`, `closed`, `truncated`, and `frame_bytes`. When a datagram or frame is
larger than `max_bytes`, its retained prefix is returned with `truncated=True`;
the rest of that message is discarded so the next receive starts at the next
message. WebSocket types are `continuation`, `text`, `binary`, `close`, `ping`,
`pong`, or `unknown`.

These APIs are clients only. They do not expose TCP listen/accept, UDP
multicast, custom WebSocket headers, custom certificate stores, or a raw socket
object. WebSocket URLs support DNS names or IPv4 hosts, optional ports, paths,
and query strings; URL credentials, fragments, and IPv6 literals are rejected.

Example:

```python
import solaros

print(solaros.net.ping("example-host", 4))

handle = solaros.net.websocket_connect("wss://example.com/events")
try:
    solaros.net.websocket_send(handle, b'{"ready":true}', True)
    frame = solaros.net.websocket_receive(handle, 4096, 5000)
    if frame is not None:
        print(frame["type"], frame["data"])
finally:
    solaros.net.close(handle)
```

## `solaros.ftp`

The package-gated FTP client uses synchronous, unencrypted IPv4 FTP with
passive data connections. Each call connects, logs in, performs one operation,
and disconnects:

- `list(host[, path[, username[, password[, port]]]])`
- `download(host, remote_path, local_path[, username[, password[, port]]])`
- `upload(host, local_path, remote_path[, username[, password[, port]]])`
- `mkdir(host, path[, username[, password[, port]]])`
- `rmdir(host, path[, username[, password[, port]]])`
- `remove(host, path[, username[, password[, port]]])`
- `rename(host, old_path, new_path[, username[, password[, port]]])`

The defaults are path `/`, username `anonymous`, password `solaros@`, and port
`21`. `list()` returns dictionaries with `name`, `is_directory`, and `size`.
Local paths use the normal SolarOS storage resolver. Calls raise `OSError` on
DNS, login, protocol, filesystem, cancellation, or transfer failure. FTP does
not encrypt credentials or file content; use it only on a trusted network.

```python
import solaros

for item in solaros.ftp.list("fileserver", "/incoming"):
    print(item["name"], item["size"])

solaros.ftp.upload("fileserver", "/notes/todo.txt", "/incoming/todo.txt")
```

## `solaros.ssh_keys`

SSH key functions manage the default SolarOS SSH key pair.

- `default_paths()`: return private and public key paths.
- `default_exists()`: return whether both default key files exist.
- `status()`: return key existence, sizes, and paths.
- `public_key()`: return the default OpenSSH public-key line without its newline.
- `generate([bits[, overwrite]])`: generate RSA keys.
- `remove()`: remove the default key pair.

Example:

```python
import solaros

if not solaros.ssh_keys.default_exists():
    solaros.ssh_keys.generate()

print(solaros.ssh_keys.status())
print(solaros.ssh_keys.public_key())
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

Application functions inspect the built-in foreground app registry.

- `list()`: return registered apps with `name` and `summary`.
- `find(name)`: return one app dictionary or `None`.

Example:

```python
import solaros

for app in solaros.apps.list():
    print(app["name"], "-", app["summary"])
```

## `solaros.input`

Foreground scripts can receive the generic pointer and axis events routed to
their active session. `sources()` lists registered input sources with `source`,
`name`, `source_class`, `source_class_name`, `capabilities`, and `ready`.

- `read([timeout_ms])`: return the next pointer or axis event dictionary, or
  `None`. The maximum timeout is 60000 ms.
- `clear()`: discard queued pointer and axis events and return the number
  discarded.
- `status()`: return `available`, `queued`, `capacity`, and cumulative
  `dropped` counters.

Pointer dictionaries have `type="pointer"`, source metadata, `pointer_id`,
numeric and named `mode`/`action`, `x`, `y`, `delta_x`, `delta_y`, `buttons`,
and `target`. Touch and other absolute sources use `x`/`y`; relative mice use
the deltas. Axis dictionaries have `type="axis"`, source metadata, numeric and
named `axis`, `value`, and `delta`.

```python
import solaros
from solaros import input as device_input

device_input.clear()
while not solaros.should_exit():
    event = device_input.read(100)
    if event is None:
        continue
    if event["type"] == "pointer":
        if event["mode"] == device_input.MODE_ABSOLUTE:
            print("touch", event["action_name"], event["x"], event["y"])
        else:
            print("mouse", event["delta_x"], event["delta_y"], event["buttons"])
    else:
        print("axis", event["axis_name"], event["value"], event["delta"])
```

The queue holds 16 events. When it is full, the oldest event is discarded so
the script receives current pointer state; inspect `status()["dropped"]` when
loss matters. Event reads are available only to a foreground Python app. Agent
or other headless source runners report `available=False` and return `None`.
Keyboard characters and navigation keys remain available through
`solaros.tui.getch()`.

## `solaros.tui`

TUI functions provide a small curses-like text UI layer over the SolarOS terminal. Drawing calls are queued onto the foreground UI side, so Python scripts do not write terminal memory directly.

Attributes:

- `NORMAL`
- `BOLD`
- `INVERSE`

Functions:

- `rows()`: return terminal rows.
- `cols()`: return terminal columns.
- `size()`: return `(rows, cols)`.
- `clear()`: clear the terminal.
- `refresh()`: atomically commit the buffered TUI frame and flush it.
- `move(row, col)`: move the terminal cursor.
- `write(text[, attr])`: write at the current cursor.
- `addstr(row, col, text[, attr])`: move and write text.
- `putch(row, col, ch[, attr])`: draw one character or codepoint.
- `hline(row, col, width[, attr])`: draw a horizontal line.
- `vline(row, col, height[, attr])`: draw a vertical line.
- `vrule(row, col, height[, width[, attr]])`: draw a continuous pixel vertical rule.
- `box(row, col, height, width[, attr])`: draw a box.
- `fill(row, col, height, width[, ch[, attr]])`: fill a rectangle.
- `getch([timeout_ms])`: return a key code or `None`.

Common key constants include `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`,
`KEY_CTRL_LEFT`, `KEY_CTRL_RIGHT`, `KEY_HOME`, `KEY_END`, `KEY_DELETE`,
`KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Example:

```python
import solaros
from solaros import tui

rows, cols = tui.size()
tui.clear()
tui.box(0, 0, rows, cols)
tui.addstr(1, 2, "SolarOS TUI", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit():
    key = tui.getch(250)
    if key == tui.KEY_ESCAPE:
        break
```

## `solaros.gfx`

Graphics functions provide queued access to the SolarOS foreground graphics
service. Call `begin()` before drawing and `refresh()`/`present()` to push the
frame to the display. With no argument, `begin()` uses the display framebuffer
of the shell that launched the script. A port or headless shell has no such
framebuffer, so targetless `begin()` raises `RuntimeError` instead of silently
drawing nowhere. `begin(target)` claims a verified named display target, such
as one returned by `solaros.expansion.devices()`, until `end()` or script
cleanup.

Colors:

- `WHITE`
- `LIGHT`
- `DARK`
- `BLACK`
- `GRAY_MAX`: maximum grayscale level accepted by `gray(level)`, currently `16`.

`gray(level)` returns a semantic shade from the `setterm foreground` color at
level `0` to the `setterm background` color at `GRAY_MAX`; `BLACK`, `DARK`,
`LIGHT`, and `WHITE` use the same theme range. `rgb(red, green, blue)` returns
an explicit RGB color from three `0..255` components. Color-capable TFT targets
preserve explicit RGB values in an indexed-color canvas. One-bit targets keep
the existing luminance and ordered-dither path.

Fonts:

- `FONT_SMALL`
- `FONT_MONO`
- `FONT_BOLD`
- `FONT_MONO_12`, `FONT_MONO_14`, `FONT_MONO_16`, `FONT_MONO_18`, `FONT_MONO_20`
- `FONT_BOLD_12`, `FONT_BOLD_14`, `FONT_BOLD_16`, `FONT_BOLD_18`, `FONT_BOLD_20`
- `FONT_ITALIC_12`, `FONT_ITALIC_14`, `FONT_ITALIC_16`, `FONT_ITALIC_18`, `FONT_ITALIC_20`
- `FONT_BOLD_ITALIC_12`, `FONT_BOLD_ITALIC_14`, `FONT_BOLD_ITALIC_16`, `FONT_BOLD_ITALIC_18`, `FONT_BOLD_ITALIC_20`

Italic constants currently map to the closest upright face in the trimmed firmware font set.

Functions:

- `begin([target])`: enter foreground graphics mode; without a target, require
  the current shell to have a display framebuffer; when `target` is provided,
  claim and draw to that named display target.
- `end()`: leave graphics mode and redraw the terminal.
- `width()`: return graphics width in pixels.
- `height()`: return graphics height in pixels.
- `size()`: return `(width, height)`.
- `clear([color])`: clear the graphics buffer, defaulting to `WHITE`.
- `gray(level)`: return a grayscale color value from `0` to `GRAY_MAX`.
- `rgb(red, green, blue)`: return an RGB color; each component is `0..255`.
- `color([color])`: get or set current drawing color.
- `set_color(color)`: alias for `color(color)`.
- `font([font])`: get or set current text font.
- `set_font(font)`: alias for `font(font)`.
- `pixel(x, y)`: draw one pixel.
- `line(x0, y0, x1, y1)`: draw a line.
- `rect(x, y, width, height)`: draw a rectangle outline.
- `fill_rect(x, y, width, height)`: draw a filled rectangle.
- `circle(x, y, radius)`: draw a circle outline.
- `fill_circle(x, y, radius)`: draw a filled circle.
- `bitmap(x, y, width, height, data)`: draw a transparent packed 1-bit XBM.
- `sprite(x, y, width, height, data)`: alias for `bitmap()`.
- `text(x, baseline_y, text)`: draw UTF-8 text.
- `refresh()`: present the graphics buffer.
- `present()`: alias for `refresh()`.
- `getch([timeout_ms])`: return a key code or `None`.

Bitmap and sprite rows are packed least-significant bit first, with
`(width + 7) // 8` bytes per row. Set bits draw in the current color and clear
bits remain transparent. One call accepts at most 128 packed bytes, enough for
a 32 by 32 sprite.

Example:

```python
import solaros
from solaros import gfx

gfx.begin()
w, h = gfx.size()
gfx.clear(gfx.WHITE)
gfx.color(gfx.BLACK)
gfx.rect(8, 8, w - 16, h - 16)
gfx.font(gfx.FONT_BOLD)
gfx.text(24, 36, "SolarOS Graphics")
gfx.color(gfx.gray(12))
gfx.fill_circle(w // 2, h // 2, 36)
gfx.color(gfx.BLACK)
gfx.circle(w // 2, h // 2, 36)
gfx.refresh()

while not solaros.should_exit():
    key = gfx.getch(250)
    if key == gfx.KEY_ESCAPE:
        break

gfx.end()
```

For an attached auxiliary display, first verify its ready target name with
`solaros.expansion.devices()`, then pass that name:

```python
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx.end()
```

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

## `solaros.dsp`

`solaros.dsp` accepts native little-endian signed 16-bit buffers and provides
`backend`, `capabilities`, `dot`, `gain`, `mix`, `clip`, `level`, `window`,
`fir`, and `fft`. Stateless output operations return a new `bytearray`.
Streaming constructors return objects with explicit `reset()` and `close()`
methods. See [Digital signal processing](dsp.md) for the fixed-point contract,
limits, and examples.

## Not Exposed Yet

The Python bridge intentionally does not expose raw SSH/SCP session handles yet. Those APIs need object lifetime, ownership, and event-loop rules before they can safely become scriptable.

## Quick reference

Import `solaros` and use its service tables for storage, time, networking,
hardware, jobs, sessions, input, TUI, and graphics. Foreground pointer and axis
events use solaros.input sources, read, clear, and status; keyboard characters
use solaros.tui.getch(). APIs return `None` or raise `OSError` as documented.
Long-running programs must yield cooperatively and release opened buses,
graphics targets, and other resources in `finally`.
