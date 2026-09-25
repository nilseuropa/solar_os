+++
id = "lua.ble"
title = "Lua bluetooth API"
section = "api"
summary = "Bluetooth: ble"
keywords = "lua solaros api ble ble"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua bluetooth API

[API overview](lua.md) · [Python bluetooth](python.ble.md)

## `solaros.ble`

- `solaros.ble`: keyboard `status` (including the latest reported keyboard
  battery percentage when available), `connected`, `pair`, `forget`, `layout`,
  `read`; generic client functions under `solaros.ble.gatt`, application
  peripheral functions under `solaros.ble.server`, and typed peripheral HID
  under `solaros.ble.hid` when BLE support is compiled

`solaros.ble.pair()` forgets the remembered keyboard and its bond before it
starts pairing a replacement. Pairing replaces a conflicting stale keyboard
bond from an older SolarOS release.

## BLE scan

`solaros.ble.scan()` returns a one-based array of device tables with the same
fields and blocking behavior as [Python scan](python.ble.md#solarosblescan).
It includes non-keyboard devices, returns an empty table if none are found,
and raises a Lua error on scan failure. Call it before opening GATT connections.

```lua
for _, device in ipairs(solaros.ble.scan()) do
    print(device.address, device.addr_type, device.name, device.rssi)
end
```

## Generic BLE GATT client

`solaros.ble.gatt` mirrors the [Python GATT client](python.ble.md#solarosblegatt):
`capacity()`, `connect(address, addr_type, timeout_ms)`, `disconnect(peer)`,
`status(peer)`, `services(peer)`, `characteristics(peer, service_index)`,
`read(peer, handle, timeout_ms)`, and
`write(peer, handle, data, with_response, timeout_ms)`,
`subscribe(peer, handle, indicate, timeout_ms)`,
`unsubscribe(peer, handle, timeout_ms)`, `configure_queue(peer, capacity)`, and
`poll(peer)`. Connect returns an opaque
peer handle; capacity reports the total configured generic-peer budget, not
currently free slots. Optional trailing arguments
may be omitted or `nil`; defaults are public address type, service-default
timeout, and writes with response. Use dot calls, not colon method syntax.

Lua data uses binary strings, including embedded zero bytes. Discovery results
are one-based Lua arrays, but each service's `index` field is **zero-based**;
pass that field to `characteristics()`. Status and discovery fields, service
limits, timeouts, retirement, and reconnect rules match Python. Errors raise Lua
errors; cancellation reports `BLE operation cancelled`.

Subscribe discovers the CCCD and waits for its write acknowledgement. `indicate`
defaults to `false` (notifications); use `true` for indications. `poll(peer)` is
nonblocking and returns `nil` or `{handle=..., data=..., indication=...}`; `data`
is a binary Lua string. No interpreter callback runs on the Bluetooth task.
The per-peer queue defaults to 16 entries on first subscribe. Configure a positive
capacity while connected, idle and with an empty queue. Failed allocation leaves
the old queue intact. Full queues drop new events; values over 128 bytes are
dropped whole. Status reports `event_capacity`, `event_count`, and the saturating
`events_dropped` counter. Indication confirmation acknowledges protocol receipt,
not app consumption. Disconnect, timeout, cancellation and sleep discard queued
events; reconnect and subscribe again after resume. Successful unsubscribe
discards queued events only for its characteristic. Other peers are unaffected.

Lua owns a separate session, automatically closed on interpreter exit, errors,
or stop, closing all its peers. Multiple peers can coexist within the configured
host/controller capacity, with one connection reserved for the keyboard.
Connect peers sequentially; their subsequent operations are independent.
Capacity exhaustion reports `BLE connection capacity exhausted`, and allocation
can fail without disturbing existing peers. A caught error or completion of one
REPL command does not close the interpreter's session. Use `disconnect(peer)`
when finished with a peer.

## Native BLE HID peripheral

`solaros.ble.hid` mirrors the
[Python BLE HID API](python.ble.md#solarosblehid). `start(name)` publishes a
fixed encrypted and bonded composite keyboard, mouse, and gamepad service for
one host. `pair()` explicitly forgets the next connecting peer's old HID bond
before security; call it only for a user-requested new-host pairing flow.
`status()` returns connection, security, subscription, keyboard LED,
and queue state. `poll()` returns `nil` or a `connected`, `secured`,
`disconnected`, `keyboard-leds`, or `passkey` event. Render a passkey as six
digits and enter it on the remote host.

```lua
local hid = solaros.ble.hid
hid.start("SolarOS Controls")
hid.pair()

while not solaros.should_exit() do
    local event = hid.poll()
    if event then
        print(event.type, event.status)
        if event.type == "passkey" then
            print(string.format("Enter %06d on the remote host", event.passkey))
        end
    end
    if hid.status().gamepad_subscribed then
        hid.gamepad.axis(hid.AXIS_X, -12000)
        hid.gamepad.button(1, true)
        hid.gamepad.hat(hid.HAT_UP)
        hid.gamepad.send()
        break
    end
end
```

Keyboard calls are `keyboard.press`, `keyboard.release`, and
`keyboard.release_all`. Mouse calls are `mouse.move` and `mouse.button`.
Gamepad setters accept axes in `-32768..32767`, buttons `1..32`, and `HAT_*`
directions, then `gamepad.send()` publishes the combined state. Input operations
require the corresponding encrypted host subscription. The event queue holds
16 entries; inspect `events_dropped`. `stop()` neutralizes reports and retires
the service. Runtime teardown does the same automatically.

The native HID service shares the single application-peripheral lease with
`solaros.ble.server`; the two cannot be active at once. It is separate from the
dormant USB-only `solaros.hid` package. Scripts select behavior through typed
operations and constants; report descriptors and raw report bytes stay native.

`solaros.ble.server` mirrors the [application peripheral API](../ble-server.md):
`create`, `service`, `characteristic`, `start`, `stop`, `close`, `status`, `peers`,
`set`, `send`, `disconnect`, and `poll`. Values and event `data` use binary Lua
strings; `poll()` returns `nil` when empty. Properties combine `solaros.ble.READ`,
`WRITE`, `WRITE_NO_RESPONSE`, `NOTIFY`, and `INDICATE`. The server belongs to this
runtime and is cleaned up alongside its outgoing peers on interpreter exit.

```lua
local gatt = solaros.ble.gatt
local peer = gatt.connect("aa:bb:cc:dd:ee:ff", 1) -- replace address and type
for _, service in ipairs(gatt.services(peer)) do
    print(service.uuid, service.index)
    for _, characteristic in ipairs(gatt.characteristics(peer, service.index)) do
        print(characteristic.uuid, characteristic.handle)
    end
end
-- Use a discovered handle: local data = gatt.read(peer, handle)
-- Binary write: gatt.write(peer, handle, string.char(0, 255), true)
gatt.disconnect(peer)
```

For an already connected `peer` and a discovered notification-capable `handle`:

```lua
gatt.configure_queue(peer, 32)
gatt.subscribe(peer, handle) -- third argument true selects indications
local event = gatt.poll(peer) -- call regularly from the application's loop
if event then print(event.handle, event.data, event.indication) end
print(gatt.status(peer).events_dropped)
-- When finished: gatt.unsubscribe(peer, handle)
```

## Quick reference

Use `solaros.ble` for bluetooth.
See `man lua` for runtime conventions and service availability.
