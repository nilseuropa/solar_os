+++
id = "python.network"
title = "Python networking API"
section = "api"
summary = "Networking: wifi, mqtt, http, net, ftp, ssh_keys"
keywords = "python solaros api network wifi mqtt http net ftp ssh_keys"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python networking API

[API overview](python.md) · [Lua networking](lua.network.md)

## `solaros.wifi`

Wi-Fi functions expose station, SoftAP, scan, internet sharing, NAT, and L2 IPv4 repeater controls.

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
- `nat(enabled)`: persistently enable or disable NAT from SoftAP to the active SolarOS uplink.
- `share_start()`: start the saved SoftAP and share the active uplink through IPv4 NAT.
- `share_stop()`: disable internet sharing and stop the SoftAP.
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

## Quick reference

Use `solaros.wifi`, `solaros.mqtt`, `solaros.http`, `solaros.net`, `solaros.ftp`, `solaros.ssh_keys` for networking.
See `man python` for runtime conventions and service availability.
