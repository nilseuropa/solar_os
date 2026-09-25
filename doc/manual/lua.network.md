+++
id = "lua.network"
title = "Lua networking API"
section = "api"
summary = "Networking: wifi, mqtt, http, net, ftp, sftpsync, ssh_keys"
keywords = "lua solaros api network wifi mqtt http net ftp sftpsync ssh_keys"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua networking API

[API overview](lua.md) · [Python networking](python.network.md)

## `solaros.wifi`

- `solaros.wifi`: `status`, `status_text`, `start`, `stop`, `connect`, `connect_saved`, `disconnect`, `forget`, `forget_ssid`, `forget_all`, `known`, `scan`, `ap_start`, `ap_stop`, `nat`, `repeater_start`, `repeater_stop` when Wi-Fi support is compiled

## `solaros.mqtt`

- `solaros.mqtt`: `status`, `connect`, `disconnect`, `publish`, `subscribe`, `read` when `network.mqtt` is compiled

## `solaros.http`

- `solaros.http`: bounded requests, retained same-origin sessions, and streaming handles when `network.http-client` is compiled

## `solaros.ftp`

- `solaros.ftp`: passive-mode list, download, upload, directory, delete, and rename operations when `network.ftp` is compiled

## `solaros.sftpsync`

- `solaros.sftpsync`: incremental file and directory synchronization over SFTP when `network.sftpsync` is compiled

## `solaros.net`

- `solaros.net.router_start()` and `router_stop()` control OS router mode.

- `solaros.net`: `ping`, managed `tcp_connect`, `tcp_send`, `tcp_receive`, `udp_open`, `udp_send`, `udp_receive`, `websocket_connect`, `websocket_send`, `websocket_receive`, `close`, `close_all`, and `limits` when `network.base` is compiled

## `solaros.ssh_keys`

- `solaros.ssh_keys`: `default_paths`, `default_exists`, `status`, `public_key`, `generate`, `remove` when `network.ssh` is compiled

## Managed TCP, UDP, and WebSocket clients

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

## HTTP requests

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
queued events per stream. A full queue backpressures the native worker until
the script drains it or cancels the stream; body bytes are never dropped.
Streams close at interpreter teardown; close them explicitly to release
resources promptly. Protocol records such as SSE messages can cross data-event
boundaries and must be reassembled by the script.

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

## FTP operations

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

## SFTP synchronization

`solaros.sftpsync.sync(source, destination[, recursive[, dry_run[, port[, password]]]])`
synchronizes one local path with one `[user@]host:path` endpoint. The direction
follows the operand order. Local paths use the SolarOS storage resolver. The
username defaults to the SolarOS identity, the port defaults to `22`, and an
empty password allows SSH key authentication.

The call blocks while it transfers, but remains cancellation-aware. It skips
regular files whose size and modification time match, never deletes
destination-only files, and limits recursive traversal to eight directory
levels. Set `recursive` for directories or `dry_run` to report what would
change. The returned table contains `files_changed`, `bytes_transferred`, and
`dry_run`; failures raise a Lua error.

```lua
local result = solaros.sftpsync.sync(
    "/notes", "backup@example.com:/srv/terminal/notes", true
)
print(result.files_changed, result.bytes_transferred)
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

## Quick reference

Use `solaros.wifi`, `solaros.mqtt`, `solaros.http`, `solaros.net`, `solaros.ftp`, `solaros.sftpsync`, `solaros.ssh_keys` for networking.
See `man lua` for runtime conventions and service availability.
