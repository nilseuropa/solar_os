+++
id = "python.ble"
title = "Python bluetooth API"
section = "api"
summary = "Bluetooth: ble"
keywords = "python solaros api ble ble"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python bluetooth API

[API overview](python.md) · [Lua bluetooth](lua.ble.md)

## `solaros.ble`

Top-level BLE functions expose keyboard pairing and layout controls. Generic
characteristic I/O is separate, under `solaros.ble.gatt`.

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

## `solaros.ble.hid`

Available when BLE support is compiled. This typed peripheral API makes the
SolarOS device advertise as one composite BLE keyboard, mouse, and gamepad. The
OS owns the fixed HID-over-GATT report map, descriptors, encryption, bonding,
and teardown; scripts cannot publish arbitrary HID descriptors or report bytes.

```python
hid = solaros.ble.hid
hid.start("SolarOS Controls")
# Call only after an explicit user action to pair a new host.
hid.pair()

while not solaros.should_exit():
    event = hid.poll()
    if event is not None:
        print(event)
        if event["type"] == "passkey":
            print("Enter %06d on the remote host" % event["passkey"])
    if hid.status()["keyboard_subscribed"]:
        hid.keyboard.press(hid.KEY_LEFT_CTRL, hid.KEY_A)
        hid.keyboard.release_all()
        break
```

- `start(name)`: acquire the application-peripheral lease and start connectable
  advertising. The name is 1..26 bytes without an embedded NUL. One remote host
  can connect at a time.
- `pair()`: enter explicit new-host pairing mode. SolarOS removes the next
  connecting peer's stored HID bond before starting security. Omit this on
  normal startup so a remembered host reconnects without pairing again.
- `stop()`: send best-effort neutral keyboard, mouse, and gamepad reports, stop
  advertising, disconnect the host, and retire the service.
- `status()`: return `registered`, `advertising`, `closing`, `connected`,
  `encrypted`, `bonded`, `keyboard_subscribed`, `mouse_subscribed`,
  `gamepad_subscribed`, `keyboard_leds`, and event queue counters.
- `poll()`: nonblocking; return `None` when empty or an event dictionary. Event
  types are `connected`, `secured`, `disconnected`, `keyboard-leds`, and
  `passkey`. Render a passkey as six digits and enter it on the remote host.
- `keyboard.press(*keys)`, `keyboard.release(*keys)`, and
  `keyboard.release_all()` use the `KEY_*` constants. Up to six ordinary keys
  plus modifiers can be held.
- `mouse.move(x, y)` sends signed relative movement, chunking large deltas;
  `mouse.button(mask, pressed)` uses the `MOUSE_*` masks.
- `gamepad.axis(axis, value)`, `gamepad.button(number, pressed)`, and
  `gamepad.hat(direction)` update state; `gamepad.send()` publishes it. Axes use
  `-32768..32767`, buttons are `1..32`, and hats use the `HAT_*` constants.

Input report operations require an encrypted host subscription for that report
type and otherwise raise `OSError("ESP_ERR_INVALID_STATE")`. Keyboard LED bits
are available as `LED_NUM_LOCK`, `LED_CAPS_LOCK`, `LED_SCROLL_LOCK`,
`LED_COMPOSE`, and `LED_KANA`. Call `poll()` regularly and inspect
`events_dropped`; the native queue holds 16 events and drops new events when
full. The runtime owns this service and cleans it up on normal exit, uncaught
exceptions, interruption, or forced stop.

`solaros.ble.hid` and `solaros.ble.server` share one connectable-advertising
lease, so a runtime cannot use both simultaneously. Outgoing
`solaros.ble.gatt` peers remain separate subject to the shared controller
connection budget. This BLE API is independent of the dormant USB-only
`solaros.hid` package described in [Python input](python.input.md#solaros-hid).

## `solaros.ble.scan()`

Available when BLE support is compiled. Returns a list of up to 32 device
dictionaries, including non-keyboard devices. Each record contains `address`
(colon-separated hex), `name` (possibly empty), integer `addr_type`, `rssi`
(dBm), `appearance`, and booleans `hid_service`, `keyboard_like`,
`remembered`, and `connected`. Address types are 0 public, 1 random,
2 public identity, and 3 random identity.

Uses the same blocking scan as shell `ble scan`, with no arguments. Scan errors
raise `OSError`; no devices returns an empty list. Scanning is rejected during
sleep preparation or while a generic GATT link is active or retiring. Scan
before connecting. Script cancellation is checked before and after the blocking
service call; it does not interrupt the radio scan.

```python
for device in solaros.ble.scan():
    print(device["address"], device["addr_type"], device["name"], device["rssi"])
```

## `solaros.ble.gatt`

Available when BLE support is compiled. This synchronous client owns one session
per Python runtime; Lua and the shell have separate owners. Each runtime may
own multiple peers, with independent operations and connection state.

- `capacity()`: total configured generic-peer capacity, not currently free slots.
  One additional connection is reserved for the OS keyboard. Capacity is set by
  firmware host/controller configuration, not an application peer-count limit.
- `connect(address, addr_type=0, timeout_ms=0)`: connect, discover services, and
  return an opaque peer handle.
  Use a colon-separated address such as `aa:bb:cc:dd:ee:ff`; address types are
  `0` public, `1` random, `2` public identity, and `3` random identity. Use
  `solaros.ble.scan()` to find the address and type.
- `disconnect(peer)`: invalidate the handle and request asynchronous disconnect.
  It cannot disconnect another runtime's or the shell's peer.
- `status(peer)`: return `owner`, `address`, `addr_type`, `status`, `connected`,
  `busy`, `retiring`, `mtu`, `service_count`, `max_value_bytes`, `event_capacity`,
  `event_count`, and `events_dropped`.
- `services(peer)`: return dictionaries with `index`, `uuid`, `primary`,
  `start_handle`, and `end_handle`.
- `characteristics(peer, service_index)`: return dictionaries with `uuid`, `handle`,
  and the numeric Bluetooth `properties` bitmask. Pass the service's returned
  zero-based `index`.
- `read(peer, handle, timeout_ms=0)`: return characteristic data as `bytes`.
- `write(peer, handle, data, with_response=True, timeout_ms=0)`: write binary data.
  `data` must support the buffer protocol, for example `bytes` or `bytearray`.
  Without response, completion confirms local submission, not peer receipt.
- `subscribe(peer, handle, indicate=False, timeout_ms=0)`: discover the
  characteristic's CCCD and enable notifications, or indications when `True`.
  The characteristic must advertise the requested property. Completion waits
  for the CCCD write acknowledgement; delivery starts after that acknowledgement.
- `unsubscribe(peer, handle, timeout_ms=0)`: disable delivery and discard queued
  events for this characteristic after the CCCD write succeeds.
- `configure_queue(peer, capacity)`: allocate a queue shared by this peer's
  subscriptions. Capacity must be positive; allocation may fail. The peer must
  be connected, with no pending operation and an empty queue. Failure preserves
  the old queue. First subscribe allocates 16 entries unless configured earlier.
- `poll(peer)`: nonblocking; return `None` when empty or a dictionary with
  `handle`, binary `data` (`bytes`), and `indication` (`bool`). It may be called
  while another task is performing an operation on this peer.

Arguments are positional. Timeouts accept `0..60000` milliseconds; zero selects
12 seconds for connect or 5 seconds for read/write/subscribe/unsubscribe. Errors raise `OSError`;
cancellation reports `BLE operation cancelled`. Capacity exhaustion reports
`BLE connection capacity exhausted`; allocation can also fail. Existing peers
remain connected when another connection cannot be admitted. Connect peers
sequentially: concurrent connection establishment can report
`ESP_ERR_INVALID_STATE`. Timeout or cancellation retires only the affected
connection. Release its peer handle and connect again after teardown finishes.
Rediscover characteristic handles after every reconnect, including after sleep.

The runtime closes its session and all peers on normal exit, uncaught exceptions, and stop.
Waiting operations check script stop/deadline state every 50 ms. In the REPL,
the session lasts until the interpreter exits; use `disconnect(peer)` when done.
An explicitly caught error does not end the runtime or release its session.

The current service retains at most 24 services and 64 characteristics per
service. Reads return at most the first 128 bytes; writes accept 1..128 bytes
and must fit within the negotiated MTU minus three bytes. `mtu` reports
the negotiated value; connection setup performs MTU exchange before discovery.
Notification queues preserve arrival order. Full queues drop new events;
payloads exceeding 128 bytes are dropped whole, never truncated. Both increment
the saturating `events_dropped` counter. NimBLE confirms indications at the
protocol layer; confirmation does not mean the application consumed the event.
Poll and monitor loss counters regularly. Disconnect, cancellation, timeout and
sleep release queue storage and discard queued events. Loss counters remain
readable for the peer handle's lifetime. Reconnect and resubscribe after sleep.
No user callback runs on the Bluetooth task. There is no script MTU setter or
automatic write chunking. Application services and advertising use
[`solaros.ble.server`](../ble-server.md), independently of these client peers.

```python
import solaros

gatt = solaros.ble.gatt
peers = []
try:
    # Replace both addresses and address types with your peripherals.
    peers.append(gatt.connect("aa:bb:cc:dd:ee:01", 1))
    peers.append(gatt.connect("aa:bb:cc:dd:ee:02", 1))
    for peer in peers:
        print(gatt.status(peer))
        for service in gatt.services(peer):
            print(service)
            print(gatt.characteristics(peer, service["index"]))
        # Use a readable handle from this peer: print(gatt.read(peer, handle))
finally:
    for peer in peers:
        gatt.disconnect(peer)
```

For an already connected `peer` and a discovered notification-capable `handle`:

```python
gatt.configure_queue(peer, 32)
gatt.subscribe(peer, handle)       # pass True as third argument for indications
event = gatt.poll(peer)           # call regularly from the application's loop
if event is not None:
    print(event["handle"], event["data"], event["indication"])
print(gatt.status(peer)["events_dropped"])
# When finished: gatt.unsubscribe(peer, handle)
```

## Quick reference

Use `solaros.ble` for bluetooth.
See `man python` for runtime conventions and service availability.
