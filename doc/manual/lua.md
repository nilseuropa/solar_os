+++
id = "lua"
title = "Lua API reference"
section = "api"
summary = "Complete Lua service API, conventions, and examples"
aliases = ["lua.api"]
keywords = "lua solaros api storage wifi gpio buses gfx tui examples"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# SolarOS Lua API

SolarOS embeds Lua as the `lua` foreground application. It can run an interactive REPL or execute `.lua` files from storage.

The SolarOS API is preloaded as the global table `solaros`. A minimal `require("solaros")` shim is also provided:

```lua
local solaros = require("solaros")

print("SolarOS " .. solaros.version())
print(solaros.identity.format())
```

Lua allocations prefer PSRAM. Host-facing Lua `io`, `os`, and dynamic package loading are intentionally not opened; scripts should use SolarOS services for hardware, storage, networking, and foreground UI.

## Top-Level Helpers

- `solaros.write(text)`: write to the foreground terminal.
- `solaros.version()`: return the firmware version.
- `solaros.should_exit()`: return whether the foreground app was asked to exit.
- `solaros.tick_interval([ms])`: get or set the foreground event-pump interval in milliseconds. Pass `0` to restore the 25 ms default.
- `solaros.battery_status()`: short battery status table or `nil` when battery support is compiled.
- `solaros.wifi_status()`: short Wi-Fi status table when Wi-Fi support is compiled.
- `solaros.environment()`: temperature and humidity table or `nil` when environmental sensor support is compiled.

For example, `solaros.tick_interval(5)` lets a foreground Lua app drain
terminal, TUI, and graphics events at a best-effort 5 ms cadence. It does not
schedule or preempt Lua code, and it is not a hard-real-time timer. The setting
lasts for the current foreground Lua app only; headless script jobs cannot
change it.

## Service Tables

Lua mirrors the Python `solaros` module structure:

The Lua runtime package requires PSRAM. Hardware and network tables are present
only when the board/flavor includes the corresponding service package. For
example, an ODROID-GO full build includes Lua with `solaros.spi` and
`solaros.onewire`, while omitting `solaros.adc` and `solaros.i2c` because those
service packages are not available on that board.

- `solaros.storage`: `status`, `is_mounted`, `mount`, `unmount`, `mount_point`, `usage`, `resolve`, `read_file`, `rescan`, `blocks`, `block_count`, `block`, `usage_for_block`, `mkdir`, `rmdir`, `remove`, `rename`, `copy`, `mount_volume`, `unmount_volume`
- `solaros.time`: `uptime_ms`, `sleep_ms`, `uptime`, `datetime`, `utc_datetime`, `set_datetime`, `set_utc_datetime`, `utc_to_local`, `local_to_utc`, `is_valid`, `timezone`, `set_timezone`, `ntp_sync`. `sleep_ms` is cancellation-aware and accepts delays up to one hour.
- `solaros.battery`: `status` when battery support is compiled
- `solaros.sensors`: `environment` when environmental sensor support is compiled
- `solaros.wifi`: `status`, `status_text`, `start`, `stop`, `connect`, `connect_saved`, `disconnect`, `forget`, `forget_ssid`, `forget_all`, `known`, `scan`, `ap_start`, `ap_stop`, `nat` when Wi-Fi support is compiled
- `solaros.mqtt`: `status`, `connect`, `disconnect`, `publish`, `subscribe`, `read` when `network.mqtt` is compiled
- `solaros.http`: bounded requests, retained same-origin sessions, and streaming handles when `network.http-client` is compiled
- `solaros.ftp`: passive-mode list, download, upload, directory, delete, and rename operations when `network.ftp` is compiled
- `solaros.hid`: typed `keyboard`, `mouse`, and `gamepad` tables when `service.hid` is compiled
- `solaros.gpio`: constants `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`; functions `pins`, `allowed`, `mode`, `configure`, `read`, `write`, `release` when GPIO support is compiled. Pin tables include `expansion`, `allowed`, `available`, `claimed`, `owner`, and `policy` (`free`, `releasable`, or `fixed`).
- `solaros.onewire`: `allowed`, `reset`, `scan`, `xfer` for the direct-pin compatibility API when OneWire support is compiled
- `solaros.led`: `status`, `set`, `on`, `off`, `toggle` when GPIO support is compiled
- `solaros.adc`: `pins`, `read` when ADC support is compiled
- `solaros.controls`: `list`, `get`, `set`, `create`, `delete`, `clear`, `bindings`, `bind_parameter`, `bind_midi`, `unbind` when continuous controls are compiled. Values use the normalized range `0.0..1.0`.
- `solaros.parameters`: `list`, `get`, `set` for dynamic native application parameters when continuous controls are compiled.
- `solaros.midi`: `status`, `send`, `note_on`, `note_off`, `cc`, `program`, `read`/`receive`, `close`, `streams`, `stream_add`, `stream_remove`, `stream_clear` when MIDI support is compiled.
- `solaros.osc`: `bindings`, `bind_stream`, `bind_event`, `bind_control`, `unbind`, `clear`, `encode_float`, `encode_int`, `dispatch`, `limits` when OSC support is compiled.
- `solaros.dsp`: `backend`, `capabilities`, `dot`, `gain`, `mix`, `clip`, `level`, `window`, `fir`, and `fft` when `service.dsp` is compiled. Binary strings contain native little-endian signed 16-bit values; FIR and FFT constructors return caller-owned userdata.
- `solaros.pwm`: constants `FREQ_MIN`, `FREQ_MAX`; functions `status`, `set`, `off` when PWM support is compiled
- `solaros.buses`: constants `MODE0` through `MODE3`, `SPI2_HOST`, `SPI3_HOST`, `DEFAULT_SPEED`, `MAX_SPEED`; functions `list`, `get`, `create_spi`, `attach`, `detach`, `remove`, `spi_xfer`, `spi_read`, `spi_write` when the resource service is compiled; `create_i2c`, `i2c_probe`, `i2c_scan`, `i2c_read_reg`, and `i2c_write_reg` are additionally present when I2C support is compiled; `create_onewire`, `onewire_reset`, `onewire_scan`, and `onewire_xfer` are additionally present when OneWire support is compiled; `create_ps2` is present with PS/2 support; `create_uart`, `create_midi`, `uart_write`, and `uart_read` are additionally present when UART support is compiled
- `solaros.expansion`: `drivers`, `devices`, `attach`, `detach` when the expansion service is compiled
- `solaros.neopixel`: `list`, `set`, `fill`, `show`, `clear` when the NeoPixel expansion package is compiled
- `solaros.i2c`: `info`, `probe`, `scan`, `read_reg`, `write_reg` when I2C support is compiled
- `solaros.spi`: constants `MODE0` through `MODE3`, `DEFAULT_SPEED`, and `MAX_SPEED`; functions `status`, `xfer`, `read`, `write` when SPI support is compiled
- `solaros.uart`: `status`, `baud`, `is_valid_baud`, `mode`, `write`, `read` when UART support is compiled
- `solaros.audio`: `status`, `deinit`, `off`, `set_volume`, `set_mic_gain`, `tone`, `tone_async`, `cancel`, `queue_status`, `level`, `capture`, `loopback`, `wav_info`, `record_wav`, `play_wav` when audio support is compiled. `capture(frames)` accepts 1 through 4096 frames and returns an interleaved little-endian signed-16 binary string plus a format table with `sample_format`, `sample_rate`, `channels`, and `bits_per_sample`.
- `solaros.synth`: `status`, `configure`, `configure_oscillator2`, `configure_filter`, `configure_performance`, `note_on`, `note_off`, `all_notes_off`, `stop` when synth support is compiled. It provides eight native two-oscillator voices with polyphonic or monophonic last-note playback, portamento, per-note velocity, ADSR envelopes, and resonant low-pass filters; scripts retain the system's global speaker volume. Status includes DSP-derived `pcm_peak` and `pcm_rms` values for the captured scope block.
- `solaros.ble`: `status`, `connected`, `pair`, `forget`, `layout`, `read` when BLE support is compiled
- `solaros.clipboard`: `set`, `get`, `size`, `clear`
- `solaros.identity`: `user`, `hostname`, `set_user`, `set_hostname`, `format`
- `solaros.net`: `ping`, managed `tcp_connect`, `tcp_send`, `tcp_receive`, `udp_open`, `udp_send`, `udp_receive`, `websocket_connect`, `websocket_send`, `websocket_receive`, `close`, `close_all`, and `limits` when `network.base` is compiled
- `solaros.ssh_keys`: `default_paths`, `default_exists`, `status`, `public_key`, `generate`, `remove` when `network.ssh` is compiled
- `solaros.jobs`: `list`, `count`, `status`, `start`, `stop`
- `solaros.sessions`: `create_shell`, `close`
- `solaros.apps`: `list`, `find`
- `solaros.input`: `sources`, `read`, `clear`, `status` for foreground pointer and axis events
- `solaros.contacts`: `list`, `get` when provider-neutral messaging is compiled
- `solaros.messages`: `conversations`, `list`, `send`, `mark_read`, `cancel` when provider-neutral messaging is compiled
- `solaros.tui`: curses-like terminal drawing functions
- `solaros.gfx`: foreground graphics drawing functions

Lua strings are binary-safe, so byte-oriented APIs such as `uart.read`, `i2c.read_reg`, `clipboard.get`, and `mqtt.read().payload` return Lua strings.

### Generic pointer and axis input

`solaros.input.sources()` lists registered input sources with their numeric
source, name, class, class name, capability bits, and ready state.
`read([timeout_ms])` returns the next pointer or axis event table, or `nil`; the
maximum timeout is 60000 ms. `clear()` discards queued events, and `status()`
reports `available`, `queued`, `capacity`, and cumulative `dropped` counts.

Pointer events contain source metadata, `pointer_id`, numeric and named
`mode`/`action`, `x`, `y`, `delta_x`, `delta_y`, `buttons`, and `target`.
Absolute touch sources use the coordinates; relative mice use the deltas. Axis
events contain source metadata, numeric and named `axis`, `value`, and `delta`.

```lua
local solaros = require("solaros")
local input = solaros.input

input.clear()
while not solaros.should_exit() do
    local event = input.read(100)
    if event and event.type == "pointer" then
        if event.mode == input.MODE_ABSOLUTE then
            print("touch", event.action_name, event.x, event.y)
        else
            print("mouse", event.delta_x, event.delta_y, event.buttons)
        end
    elseif event then
        print("axis", event.axis_name, event.value, event.delta)
    end
end
```

The foreground queue holds 16 events and discards the oldest event when full.
Agent and other headless source runners report `available=false` and return
`nil`. Keyboard characters and navigation keys remain available through
`solaros.tui.getch()`.

### Managed TCP, UDP, and WebSocket clients

`solaros.net` mirrors the Python managed-network API:

- `tcp_connect(host, port[, timeout_ms])`, `tcp_send(handle, data[, timeout_ms])`, and `tcp_receive(handle[, max_bytes[, timeout_ms]])`
- `udp_open([local_port])`, `udp_send(handle, host, port, data[, timeout_ms])`, and `udp_receive(handle[, max_bytes[, timeout_ms]])`
- `websocket_connect(url[, subprotocol[, timeout_ms]])`, `websocket_send(handle, data[, text[, timeout_ms]])`, and `websocket_receive(handle[, max_bytes[, timeout_ms]])`
- `close(handle)`, `close_all()`, and `limits()`

Calls are synchronous. Connect defaults to 10000 ms; send and receive default
to 1000 ms. The accepted range is 0 through 60000 ms. Receive returns `nil` on
timeout. TCP peer closure returns an empty string. Other failures raise a Lua
error. UDP results contain `data`, `address`, `port`, `truncated`, and
`datagram_bytes`; WebSocket results contain `data`, `type`, `final`, `closed`,
`truncated`, and `frame_bytes`.

Each Lua app or runner invocation owns its handles exclusively. Handles cannot
be shared with Python or another invocation, are generation checked, and close
automatically before interpreter teardown on normal exit, error, cancellation,
or forced cleanup. The combined per-invocation limit is four TCP, UDP, and
WebSocket handles, with eight script handles globally. Transfers and WebSocket
frames are limited to 65536 bytes and UDP datagrams to 65507 bytes. Oversized
messages return the retained prefix and discard the remainder of that message.

TCP and UDP waits check cancellation within 50 ms polling slices. DNS checks
cancellation before and after the platform resolver. WebSocket DNS, TCP/TLS,
upgrade, and frame operations check before and after their bounded transport
call, so cancellation can take up to the remaining call timeout. TCP and UDP
use one end-to-end deadline per public operation. A WebSocket public call can
contain multiple platform transport stages; each stage is separately bounded
by the supplied timeout, and cancellation is checked between stages.
WebSocket receive polling, reading, and truncated-frame draining share the
remaining SolarOS-layer deadline. `ws://` and certificate-validated `wss://`
clients are supported; listener/server sockets, multicast, custom WebSocket
headers, custom certificate stores, URL credentials, fragments, and IPv6
literals are not.

```lua
local handle = solaros.net.udp_open()
solaros.net.udp_send(handle, "example.com", 9000, "hello")
local packet = solaros.net.udp_receive(handle, 4096, 1000)
if packet then
    print(packet.address, packet.port, packet.data)
end
solaros.net.close(handle)
```

### HTTP requests

`solaros.http` uses the shared bounded SolarOS HTTP client. HTTPS uses the
firmware certificate bundle. The mirrored call forms are:

- `request(method, url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `get(url[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]])`
- `head(url[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]])`
- `post`, `put`, `patch`, and `delete` use
  `(url[, body[, headers[, timeout_ms[, max_bytes[, follow_redirects]]]]])`
- `session_open(origin)`
- `session_request(handle, method, url[, body[, headers[, timeout_ms[, max_bytes]]]])`
- `session_close(handle)` and `session_close_all()`
- `stream_open(method, url[, body[, headers[, timeout_ms[, follow_redirects]]]])`
- `stream_read(handle[, timeout_ms])`, `stream_close(handle)`, and
  `stream_close_all()`

URLs must use `http://` or `https://`. Headers are a table of up to 16 string
pairs and 8192 bytes total; names and values cannot contain line breaks.
Defaults are 10000 ms, a 65536-byte response body, and redirect following. `max_bytes`
accepts 0 through 262144. The response table contains `status_code`, binary
`body`, `headers`, `content_length`, `bytes_received`, `duration_ms`,
`truncated`, and `headers_truncated`. A body over the limit returns its retained
prefix with `truncated=true`. HTTP 4xx and 5xx statuses are normal responses;
request, cancellation, deadline, DNS, TLS, and transport failures raise Lua
errors. Exiting or interrupting Lua cancels an active request.

`session_open` retains one same-origin HTTP/TLS client. Its origin contains
only scheme and authority. `session_request` rejects redirects and cross-origin
URLs, clears previous request headers, and returns the same bounded response
table as `request`. A stale connection is retried once only for GET or HEAD
before any response starts; writes are never retried. Each runtime can retain
two sessions, with four globally. Session handles close automatically at
interpreter teardown, but scripts should close them explicitly.

`stream_open` runs the HTTP operation in a native worker without an end-to-end
deadline. Its timeout bounds each transport operation and accepts 0 through
60000 ms; zero selects the 10000 ms service default. `stream_read` returns
`nil` on wait timeout. Otherwise it returns an
ordered `header`, `response`, `data`, `complete`, or `error` event. Data events
contain up to 1024 binary bytes. Terminal events include status, content
length, received byte count, duration, cancellation flags, and ESP error
details. The limits are two streams per runtime and four globally, with eight
queued events per stream. A full queue terminates the stream instead of
dropping bytes. Streams close at interpreter teardown; close them explicitly
to release resources promptly. Protocol records such as SSE messages can cross
data-event boundaries and must be reassembled by the script.

```lua
local response = solaros.http.get("https://example.com/")
print(response.status_code, #response.body)

response = solaros.http.post(
    "https://example.com/api",
    '{"state":"online"}',
    { ["Content-Type"] = "application/json" }
)
print(response.status_code, response.body)
```

```lua
local handle = solaros.http.session_open("https://example.com")
local first = solaros.http.session_request(handle, "GET", "https://example.com/a")
local second = solaros.http.session_request(handle, "GET", "https://example.com/b")
solaros.http.session_close(handle)
```

### FTP operations

`solaros.ftp` uses synchronous, unencrypted IPv4 FTP. Each call connects,
performs one operation with passive data connections, and disconnects:

- `list(host[, path[, username[, password[, port]]]])`
- `download(host, remote_path, local_path[, username[, password[, port]]])`
- `upload(host, local_path, remote_path[, username[, password[, port]]])`
- `mkdir`, `rmdir`, and `remove` use
  `(host, path[, username[, password[, port]]])`
- `rename(host, old_path, new_path[, username[, password[, port]]])`

The defaults are `/`, `anonymous`, `solaros@`, and port `21`. Listings contain
`name`, `is_directory`, and `size`. Local paths use SolarOS storage resolution.
Failures raise a Lua error. FTP does not encrypt credentials or content; use it
only on a trusted network.

```lua
for _, item in ipairs(solaros.ftp.list("fileserver", "/incoming")) do
    print(item.name, item.size)
end
solaros.ftp.download("fileserver", "/incoming/report.txt", "/notes/report.txt")
```

A script-driven continuous control uses the same target mappings as an ADC
potentiometer. Lua can create, bind, inspect, and remove controls directly:

```lua
solaros.controls.create("expression")
solaros.controls.bind_parameter(
    "expression", "synth.filter.resonance", false
)
solaros.jobs.start("controls")
solaros.controls.set("expression", 0.5)
print(solaros.controls.get("expression"))
```

`solaros.controls.create(name[, source, input_min, input_max, smoothing_ms,
deadband, inverted])` omits `source` for manual controls. `bindings()` includes
pickup, application, and error state. `bind_parameter()` and `bind_midi()`
return binding IDs; `unbind()` returns the number removed.

Dynamic app parameters are available through `solaros.parameters.list()`,
`get(path)`, and `set(path, value)`. `set()` returns the authoritative
native-unit value after the parameter's range and step handling.

### MIDI

The MIDI job must own a running bus before Lua transmits or receives. The
following setup creates a bus, starts the worker, sends a note, and waits up to
one second for a non-consuming subscriber message:

```lua
solaros.buses.create_midi("midi0", { tx = 2, rx = 3 })
solaros.jobs.start("midi", { "midi0" })
solaros.midi.note_on(1, 60, 100)
local message = solaros.midi.read(1000)
```

`status()` includes traffic, parser, drop, error, CC-stream, and script
subscription state. `send(status[, data1, data2])` validates raw messages;
`note_on`, `note_off`, `cc`, and `program` provide channel-oriented helpers.
`read()` and its `receive()` alias return a message table or `nil`, are bounded
to 60 seconds, and are cancellation-aware. The subscription is automatically
released when Lua exits; `close()` releases it earlier.

Use `streams()`, `stream_add(channel, controller)`, `stream_remove(...)`, and
`stream_clear()` to manage incoming CC scalar streams. Message tables contain
`status`, `length`, `type`, and applicable channel/data fields.

### Open Sound Control

Lua configures native OSC bindings while the `osc` job retains UDP socket,
filtering, and rate-limit ownership:

```lua
solaros.osc.bind_stream(
    "ambient", "temperature", "/room/temperature", 2.0, 0.1
)
solaros.jobs.start(
    "osc", { "listen=9000", "target=192.168.1.50:9001" }
)
```

`bindings()` returns source configuration plus availability, values, timing,
send counters, and errors. `bind_stream`, `bind_event`, and `bind_control`
return numeric IDs; `unbind` and `clear` remove definitions. Event edges are
`"rising"`, `"falling"`, or `"both"`; rates are `0.1..100` Hz.

`encode_float()` and `encode_int()` return binary OSC messages that can be sent
with `solaros.net.udp_send()`. `dispatch(packet)` validates a message or
immediate bundle and applies the same native parameter routes as the job.
`limits()` reports all public codec and binding bounds.

For example, this plays a short saw-wave chord without running Lua in the
real-time render callback:

```lua
solaros.synth.configure("saw", 5, 80, 65, 140)
solaros.synth.configure_oscillator2("square", 0, 7, 35)
solaros.synth.configure_filter(1200, 35, 80, 5, 250, 20, 180)
solaros.synth.configure_performance(true, 80)
solaros.synth.note_on(440, 110)
solaros.synth.note_on(554, 90)
solaros.time.sleep_ms(250)
solaros.synth.all_notes_off()
solaros.time.sleep_ms(150)
solaros.synth.stop()
```

`note_on()` accepts 20 through 8000 Hz and velocity 1 through 127. Envelope
times accept 0 through 10000 ms and sustain accepts 0 through 100 percent. The
`configure()` updates active voices immediately and also sets the defaults for
future notes. `configure_filter()` accepts cutoff from 40 through 18000 Hz,
resonance and envelope amount from 0 through 100 percent, followed by its own
attack, decay, sustain, and release values. The first note claims exclusive
audio output lazily, and the runtime releases it automatically when the script
exits or is interrupted.
`configure_oscillator2()` accepts waveform, octave from -2 through +2, fine
detune from -100 through +100 cents, and mix from 0 through 100 percent. Mix
zero bypasses oscillator 2 exactly. Both oscillators share the filter and
envelopes.
`configure_performance()` selects polyphonic or monophonic last-note playback
and accepts a glide time from 0 through 2500 ms.

`solaros.messages.send(conversation_id, body[, allow_untrusted])` queues a
message and returns its stable hexadecimal ID. `list()` also represents message
IDs as hexadecimal strings, and `cancel(id)` accepts that representation.
Blocked direct endpoints are always rejected; discovered endpoints require the
optional boolean for that one send. `solaros.contacts` returns only contact
summaries and endpoint IDs, never credentials or endpoint secret material.

`solaros.onewire.scan(pin)` returns tables containing a 16-digit hexadecimal
`address` and numeric `family` code. `solaros.onewire.xfer(pin, read_len[, data])`
resets the bus, writes the binary-safe `data` string, and returns `read_len`
bytes. Reads and writes are each limited to 64 bytes.

## Named buses and expansion devices

`solaros.buses` discovers board-defined and runtime-created buses independently
of the legacy single-board-bus and direct-pin service tables.

- `list()` returns every bus table.
- `get(name)` returns one bus table.
- `create_i2c(name, config)` creates a runtime I2C bus and returns its table.
- `create_onewire(name, config)` creates a runtime 1-Wire bus and returns its table.
- `create_spi(name, config)` creates a runtime SPI bus and returns its table.
- `create_uart(name, config)` creates a lazy runtime UART bus and returns its table.
- `create_midi(name, config)` creates an exclusive MIDI bus and automatically selects its UART backend.
- `attach(name)` attaches a named detachable bus and reserves its endpoint and pins.
- `detach(name)` detaches an idle named bus without deleting its descriptor.
- `remove(name)` removes an idle runtime bus.
- `i2c_probe(bus, address)`, `i2c_scan(bus)`,
  `i2c_read_reg(bus, address, reg, length)`, and
  `i2c_write_reg(bus, address, reg, data)` operate on a selected named I2C bus
  when both the resource and I2C services are compiled.
- `onewire_reset(bus)`, `onewire_scan(bus)`, and
  `onewire_xfer(bus, read_len[, data])` operate on a selected registered
  OneWire bus when both the resource and OneWire services are compiled.
- `uart_write(bus, data)` and `uart_read(bus[, length[, timeout_ms]])` operate
  on a selected named UART when both the resource and UART services are compiled.
- `spi_xfer(bus, cs, data[, mode[, speed_hz]])`,
  `spi_read(bus, cs, length[, fill[, mode[, speed_hz]]])`, and
  `spi_write(bus, cs, data[, mode[, speed_hz]])` transfer on a selected named
  bus. Each raw transfer takes and releases a temporary lease automatically.

Bus tables contain `id`, `name`, `protocol`, `origin`, `sharing`, `attached`,
`detachable`, `ready`, and `lease_count`, plus protocol-specific pins and configuration. `create_spi`
requires `host`, `sclk`, `mosi`, and a one-to-four-element `cs` array. `miso`
and `max_transfer_size` are optional. I2C bus tables include `port`, `sda_pin`,
`scl_pin`, and `speed_hz`. Named I2C operations take and release a shared lease
automatically; the legacy `solaros.i2c` table remains an `i2c0` shortcut.
OneWire bus tables include `pin`. Named OneWire operations take and release an
exclusive lease automatically; `solaros.onewire` remains the direct-pin
compatibility API. UART bus tables include `port`, `tx_pin`, `rx_pin`, and
`baud_rate`; named UART I/O takes and releases an exclusive
lease automatically.

`create_i2c` requires `port`, `sda`, and `scl`; optional `speed_hz` defaults to
100000. `create_onewire` requires `pin`. Both claim their approved runtime pins
until `remove(name)`.

`create_uart` requires `port`, `tx`, and `rx`; optional `baud_rate` defaults to
115200. Runtime descriptors are detachable and removable. Board descriptors
whose signal pins are marked releasable are detachable but never removable;
fixed-pin board descriptors reject detach. Attached buses own their hardware
endpoint and signal pins, while protocol hardware starts on first lease.

`create_midi` requires `tx` and `rx`; optional `baud_rate` defaults to 31250.
SolarOS selects an unused board-approved UART controller. The returned `port`
is diagnostic backend information, not an input to the MIDI API.

```lua
local solaros = require("solaros")

local bus = solaros.buses.create_spi("spi1", {
    host = solaros.buses.SPI3_HOST,
    sclk = 1,
    mosi = 2,
    miso = 3,
    cs = {17},
})
print(bus.name, bus.origin)

local reply = solaros.buses.spi_xfer("spi1", "gpio17", "\x9f\x00\x00\x00")
print(#reply)
solaros.buses.remove("spi1")
```

```lua
local solaros = require("solaros")

local i2c1 = solaros.buses.create_i2c("i2c1", {
    port = 1,
    sda = 14,
    scl = 15,
    speed_hz = 100000,
})
print(#solaros.buses.i2c_scan(i2c1.name))
solaros.buses.remove(i2c1.name)

local onewire0 = solaros.buses.create_onewire("onewire0", {pin = 16})
print(#solaros.buses.onewire_scan(onewire0.name))
solaros.buses.remove(onewire0.name)

local uart1 = solaros.buses.create_uart("uart1", {
    port = 1,
    tx = 14,
    rx = 15,
    baud_rate = 115200,
})
solaros.buses.uart_write(uart1.name, "AT\r\n")
print(solaros.buses.uart_read(uart1.name, 64, 500))
solaros.buses.detach(uart1.name)
solaros.buses.attach(uart1.name)
solaros.buses.remove(uart1.name)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("i2c0")
print(bus.name, bus.speed_hz)
local addresses = solaros.buses.i2c_scan("i2c0")
solaros.buses.i2c_probe("i2c0", 0x3c)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("onewire0")
print(bus.name, bus.pin)
local devices = solaros.buses.onewire_scan("onewire0")
local reply = solaros.buses.onewire_xfer("onewire0", 9, "\xcc\x44")
```

`solaros.expansion.drivers()` lists compiled drivers. `devices()` lists active
devices with `name`, `driver`, `origin` (`board` or `runtime`), `ready`,
`autostart`, `detachable`, and normalized `bindings`. Each binding contains
`kind`, `role`, `target`, `value`, and `aux`. `attach(driver, name, bindings)`
and `detach(name)` mirror the shell lifecycle. Binding tables accept `spi`,
`cs` (or `ce`), `i2c`, `addr`, `uart`, `ps2`, `gpio`, `irq`, `reset` (or
`rst`), `dc`, `busy`, `data`, `bck`, `din`, `rck`, `adc`, `pwm`, `count`,
`keys`, `x`, `y`, `min`, `center`, `max`, and `deadzone`. `ps2` names an
existing PS/2 bus; `x` and `y` name scalar streams; `keys` maps logical key
names to GPIO numbers. `cs` requires `spi`, `addr` requires `i2c`, and unknown
fields are rejected.

```lua
solaros.expansion.attach("pcd8544", "lcd0", {
    spi = "spi0",
    cs = 10,
    dc = 4,
    reset = 5,
})
print(#solaros.expansion.devices())
solaros.expansion.detach("lcd0")
```

NeoPixel `set` and `fill` update a buffer; call `show` once after a batch of
changes. `clear` updates and transmits immediately.

```lua
solaros.expansion.attach("neopixel", "pixels0", {data = 1, count = 8})
solaros.neopixel.fill("pixels0", 0, 0, 8)
solaros.neopixel.set("pixels0", 3, 16, 0, 0)
solaros.neopixel.show("pixels0")
```

`solaros.spi` is a compatibility table that selects `spi0` when present,
otherwise the first registered named SPI bus. On a dynamic-only board its
`status().available` value remains false until a bus is created. `status()`
reports the selected bus pins, transfer limit, and configured chip-select
slots. `xfer(cs, data[, mode[, speed_hz]])` performs a full-duplex transaction.
`read(cs, length[, fill[, mode[, speed_hz]]])` and
`write(cs, data[, mode[, speed_hz]])` provide one-direction convenience forms.
The `cs` argument accepts a configured slot name or its numeric GPIO. Lua data
and return values are binary-safe strings. New code should address buses
explicitly through `solaros.buses.spi_*`.

### USB HID

`service.hid` is retained as a dormant package and is not compiled into the
standard SolarOS flavors because the TinyUSB composite stack currently costs
too much internal SRAM. On an ESP32-S3 build that explicitly enables it, the
same USB connection remains available as `cdc0` while also advertising
keyboard, mouse, and gamepad HID reports. Lua uses the same typed operations
and constants as Python:

```lua
local hid = solaros.hid

hid.keyboard.press(hid.KEY_LEFT_CTRL, hid.KEY_C)
hid.keyboard.release_all()
hid.mouse.move(10, -4)
hid.mouse.button(hid.MOUSE_LEFT, true)
hid.mouse.button(hid.MOUSE_LEFT, false)
hid.gamepad.axis(hid.AXIS_X, -12000)
hid.gamepad.button(1, true)
hid.gamepad.hat(hid.HAT_UP)
hid.gamepad.send()
```

Keyboard transitions are queued, mouse deltas accumulate, and gamepad state is
coalesced until `send()`. Axes use `-32768..32767`; gamepad buttons are `1..32`.
Disconnected or unavailable HID calls raise `ESP_ERR_INVALID_STATE`. SolarOS
sends neutral reports whenever the Lua runtime exits, fails, or is force-stopped.

`solaros.uart` is the default `uart0` compatibility table; use
`solaros.buses.uart_*` for another named UART and `solaros.buses.attach()` or
`detach()` for lifecycle control. `solaros.uart.status()`
includes the bus `name`, `attached`, `rx_buffered`, and `rx_buffered_valid`.
When another owner is
actively using the UART, `rx_buffered_valid` is `false` because the live RX
count is not sampled.

## Identity

`solaros.identity.user()` and `hostname()` return the NVS-backed device
identity. `set_user(name)` and `set_hostname(name)` validate and persist new
values. Reboot before expecting an already initialized Wi-Fi interface to
advertise a changed hostname.

SSH and SCP use the identity user as their default remote username when
`user@host` is not supplied.

Existing `/.solar/user` and `/.solar/hostname` files are imported once when
their corresponding NVS keys are absent.

## TUI

`solaros.tui` draws into one buffered foreground frame. `refresh()` atomically
commits the changed cells. It exposes constants
`NORMAL`, `BOLD`, `INVERSE`, plus common key constants such as `KEY_UP`,
`KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_CTRL_LEFT`, `KEY_CTRL_RIGHT`,
`KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Functions:

- `rows()`, `cols()`, `size()`
- `clear()`, `refresh()`
- `move(row, col)`, `write(text[, attr])`, `addstr(row, col, text[, attr])`
- `putch(row, col, ch[, attr])`
- `hline(row, col, width[, attr])`, `vline(row, col, height[, attr])`, `vrule(row, col, height[, width[, attr]])`
- `box(row, col, height, width[, attr])`
- `fill(row, col, height, width[, ch[, attr]])`
- `getch([timeout_ms])`

Example:

```lua
local solaros = require("solaros")
local tui = solaros.tui

tui.clear()
tui.box(0, 0, tui.rows(), tui.cols())
tui.addstr(1, 2, "SolarOS Lua", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit() do
    local key = tui.getch(250)
    if key == tui.KEY_ESCAPE then
        break
    end
end
```

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

## Graphics

`solaros.gfx` draws through the foreground graphics service. `begin()` uses the
display framebuffer of the shell that launched the script; from a port or
headless shell it raises an error because there is no foreground display.
`begin(target)` claims a verified named display target, such as one returned by
`solaros.expansion.devices()`, until `end()` or script cleanup. Colors are
`WHITE`, `LIGHT`, `DARK`, `BLACK`, `gray(level)` with `0..GRAY_MAX`, and
`rgb(red, green, blue)` with `0..255` components. On color TFT targets, the
named colors and `gray(level)` span the `setterm foreground` and `background`
theme, while `rgb(...)` remains literal. One-bit targets keep the existing
luminance and ordered-dither path. Fonts
are `FONT_SMALL`, `FONT_MONO`, `FONT_BOLD`, regular document fonts
`FONT_MONO_12` through `FONT_MONO_20`, bold document fonts `FONT_BOLD_12`
through `FONT_BOLD_20`, and matching italic/bold-italic constants. Italic
constants currently map to the closest upright face in the trimmed firmware
font set.

Functions:

- `begin([target])`, `end()`
- `width()`, `height()`, `size()`
- `clear([color])`
- `gray(level)`
- `rgb(red, green, blue)`
- `color([color])`, `set_color(color)`
- `font([font])`, `set_font(font)`
- `pixel(x, y)`, `line(x0, y0, x1, y1)`
- `rect(x, y, width, height)`, `fill_rect(x, y, width, height)`
- `circle(x, y, radius)`, `fill_circle(x, y, radius)`
- `bitmap(x, y, width, height, data)`, `sprite(...)` alias
- `text(x, baseline_y, text)`
- `refresh()`, `present()`
- `getch([timeout_ms])`

Bitmap and sprite rows are packed least-significant bit first, with
`(width + 7) // 8` bytes per row. Set bits draw in the current color and clear
bits remain transparent. One call accepts at most 128 packed bytes, enough for
a 32 by 32 sprite. Lua passes the packed bytes in a binary string.

Example:

```lua
local solaros = require("solaros")
local gfx = solaros.gfx

gfx.begin()
local w, h = gfx.size()
gfx.clear(gfx.WHITE)
gfx.color(gfx.BLACK)
gfx.rect(8, 8, w - 16, h - 16)
gfx.font(gfx.FONT_BOLD)
gfx.text(24, 36, "SolarOS Lua")
gfx.color(gfx.gray(12))
gfx.fill_circle(w // 2, h // 2, 36)
gfx.color(gfx.BLACK)
gfx.circle(w // 2, h // 2, 36)
gfx.refresh()

while not solaros.should_exit() do
    local key = gfx.getch(250)
    if key == gfx.KEY_ESCAPE then
        break
    end
end

gfx["end"]()
```

For an attached auxiliary display, first verify its ready target name, then
pass that name:

```lua
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx["end"]()
```

## Notes

For the Q15 numeric contract, processor lifetime, limits, and examples, see
[Digital signal processing](dsp.md). Lua DSP operations return new binary
strings because ordinary Lua strings are immutable.

Lua tables returned as lists use normal Lua 1-based array indexes. Direct block lookup with `solaros.storage.block(index)` follows the underlying storage service index, matching Python's 0-based `block(index)`.

The Lua bridge intentionally does not expose raw SSH/SCP session handles. Those need explicit object lifetime and event-loop rules before becoming scriptable.

## Quick reference

Load `solaros` and use its service tables for storage, time, networking,
hardware, jobs, sessions, input, TUI, and graphics. Foreground pointer and axis
events use solaros.input sources, read, clear, and status; keyboard characters
use solaros.tui.getch(). Lua arrays are 1-based unless an individual service
explicitly exposes a native index. Close resources and keep long-running loops
cooperative.
