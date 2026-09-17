# Application BLE peripheral

`solaros.ble.server` exposes an application-owned GATT server in Python and Lua.
The C entry point is `solar_os_ble_server_request(session, request)`. Its request
and event structures contain only OS types and copied data, not NimBLE callbacks.

One runtime owns the legacy connectable-advertising lease at a time. Another
runtime cannot modify or close its peripheral. A peripheral can contain multiple
primary services and characteristics; allocation is dynamic. Incoming links and
outgoing GATT peers share the configured host/controller capacity, with one link
reserved for the OS keyboard. Advertising reserves one prospective incoming slot.
It pauses when the application connection capacity is full and resumes as slots
become available. There is no separate application-selected peer-count ceiling.
The typed `solaros.ble.hid` peripheral uses the same lease, so it and the generic
application server cannot be active simultaneously.

## Python and Lua API

Arguments are positional. Errors raise `OSError` in Python or Lua errors.
Lua uses binary strings; Python values must support the buffer protocol.

- `create(name, queue_capacity=16)`: acquire the runtime's peripheral lease.
  The name is 1..26 bytes without embedded NUL. Queue capacity must be positive;
  checked allocation can fail. Creation alone does not advertise.
- `service(uuid)`: add a primary service and return an opaque local ID.
- `characteristic(service, uuid, properties, value=empty)`: add a characteristic
  and return its opaque local ID. Combine `solaros.ble.READ`, `WRITE`,
  `WRITE_NO_RESPONSE`, `NOTIFY`, and `INDICATE` with bitwise OR. Other properties
  are rejected. CCCDs are provided by NimBLE for notify/indicate characteristics.
- `start()`: register the definitions and start connectable advertising.
  The first service UUID is advertised; the complete name is in the scan response.
  Definitions become immutable after successful registration, even if advertising
  subsequently fails. Retry `start()` or `close()` and create a new definition set.
- `stop()`: stop advertising, retaining services and existing connections.
- `close()`: invalidate access and request teardown of this runtime's peripheral.
  Existing incoming links are disconnected before registered definitions are freed.
  A new `create()` can report busy until asynchronous teardown has finished.
- `status()`: return `registered`, `advertising`, `closing`, `peers`, `services`,
  `characteristics`, `event_capacity`, `event_count`, and `events_dropped`.
- `peers()`: enumerate incoming links as `{peer, mtu, connected}` records.
  This also permits recovery when a connection event was dropped. Enumeration is
  not an atomic snapshot across disconnects. Use `gatt.status()` for outgoing peers.
- `set(characteristic, data)`: replace the stored value without sending anything.
- `send(peer, characteristic, data, indicate=False)`: explicitly send a notification
  or indication. The peer must have subscribed to that mode. This does not replace
  the stored value. Indications allow one outstanding transaction per incoming link.
- `disconnect(peer)`: request disconnection of an incoming link owned by this server.
- `poll()`: return the next queued event, or `None` / `nil` when empty. This does
  not wait for radio traffic or dispatch a host command.

UUIDs accept 4 or 8 hexadecimal digits, a canonical 36-character UUID, or `0x`
followed by 4 digits. Duplicate service UUIDs in the peripheral or the live host
database are rejected. Local IDs and incoming peer IDs are not ATT handles or
outgoing `gatt` peer handles. IDs do not wrap or alias a later peripheral lease.

## Value and event semantics

Stored values and event payloads support 0..128 bytes. Sends must additionally fit
the connection's negotiated `MTU - 3`; use `peers()` to inspect the current MTU.
There is no automatic fragmentation or application MTU setter. Reads, including
Read Blob requests, return the stored value immediately. The script must update
values proactively; it cannot delay an ATT read while computing a response.
Prepared/offset writes and script-controlled ATT responses are not supported.

Each event contains `type`, `peer`, `characteristic`, `mtu`, `status`, `notify`,
`indicate`, and binary `data`. Fields unrelated to the event are zero/false/empty.

| Type | Meaning |
|---|---|
| `connected` | Incoming link admitted; `peer` identifies it and `mtu` is its initial MTU. |
| `disconnected` | Incoming link ended; `status` contains the controller reason. |
| `read` | A remote read was served from the stored value. No value payload is queued. |
| `write` | A complete remote write was accepted; `data` is an owned copy. |
| `subscribe` | CCCD state changed; `notify` and `indicate` report the enabled modes. |
| `sent` | Notification submission completed, or an indication was confirmed/failed. |

A successful `send()` confirms local acceptance, not delivery. For indications,
`sent` with `indicate=true` and `status=0` means the peer confirmed the indication.
For notifications, `status=0` does not prove application consumption. Nonzero
send status is backend diagnostic information, not a stable OS error enumeration.

The per-peripheral queue preserves arrival order and drops new events when full,
incrementing the saturating loss counter. A write which cannot be queued is
**not applied**: write-with-response receives an ATT resource error; a command
(write without response) cannot receive an ATT error response. Read/subscription/
connection/send state still advances if its informational event is dropped.
Use `status()` and `peers()` to recover state; a dropped confirmation event cannot
be reconstructed. Poll frequently and choose capacity for the application's burst.

## Ownership, security and lifecycle

Access and GAP callbacks only copy data and update native state. They never call
an interpreter. Database mutation, advertising and sends run on the host queue;
application tasks never acquire the host lock while holding the adapter lock.
Queued commands have a five-second completion timeout; an abandoned command cannot
later create or restart a peripheral. A timed-out command retires its owned server.
It cannot undo a transmission already accepted by the host. Poll/status/set operate
directly on mutex-protected copied state.

Normal interpreter exit, uncaught errors, stop and sleep discard queued events,
stop advertising and retire incoming links. Recreate the server after sleep;
definitions are not automatically republished. Closing the server leaves outgoing
GATT peers usable; closing the runtime releases both. Host shutdown is a callback
barrier before any remaining definition storage is freed. Teardown failure retains
storage and keeps the server closing rather than risking dangling SDK pointers.

Application characteristics **do not require encryption or authentication** in
this increment. Do not expose secrets or safety-sensitive controls. Pairing/bond policy,
encrypted/authenticated characteristic flags, custom descriptors, included services,
arbitrary advertising payloads and multiple advertising sets are not exposed.
The keyboard and generic outgoing links cannot access application characteristic
values through their existing central connections. Standard GAP/GATT services are
registered at host startup so remote clients can use Service Changed; clients
which ignore cache invalidation may still require their own cache refresh.

## Example

```python
import solaros

ble = solaros.ble
s = ble.server
s.create("SolarOS Sensor")
service = s.service("3ad20001-8d42-4e59-9282-5aaf9d312001")
value = s.characteristic(service, "3ad20002-8d42-4e59-9282-5aaf9d312001",
                         ble.READ | ble.WRITE | ble.NOTIFY, b"ready")
s.start()
# Keep this runtime alive while serving. Poll in the application's normal loop.
event = s.poll()
# s.set(value, b"new") updates reads; s.send(peer, value, b"new") sends explicitly.
```

For a controlled radio check, see `solar_os_test/tools/ble_server_check.py`.
Native tests cover ownership, binary values, allocation failures, queue overflow,
shared capacity, indication completion, abandoned requests, and teardown barriers.
They do not establish hardware keyboard or sleep/wake coexistence.
