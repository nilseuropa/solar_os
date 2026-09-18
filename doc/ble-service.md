# BLE sessions and backend boundary

`src/services/solar_os_ble.h` is the general BLE API. It exposes SolarOS session
handles, scan results, characteristic properties, discovery/value types, and
shared initialization and sleep/resume entry points. Bluetooth host-stack types
are confined to the backend.

## Ownership and lifetime

`solar_os_ble_session_create(owner, &session)` allocates an opaque, nonzero
session handle. Sessions and per-peer state are allocated dynamically. A separate
`ble.shell` session serves the existing `solar_os_ble_gatt_*` API. Owner names
are bounded diagnostic labels, not security credentials.

An app session may own multiple peers. `solar_os_ble_peer_connect()` returns an
opaque peer handle after connection and discovery complete. The `peer_*` data
operations and disconnect require both the owning session and peer handles.
Each peer has independent discovery, request, result, timeout and retirement
state. A timeout or disconnect on one peer does not retire the others.
The compatibility API cannot read or disconnect an app's connection. The OS
keyboard retains its separate HID connection and pairing/reconnect policy.

`solar_os_ble_peer_disconnect()` immediately invalidates the peer handle.
Session close invalidates all its peer handles and closes their connections.
Remote disconnect and sleep retain peer handles for status; release them with
peer disconnect, then connect again to obtain a fresh handle and discovery.
The older `session_*` data operations retain an implicit default connection for
C callers and the shell. They do not select an arbitrary explicit peer.

A caller keeps the handle for its lifetime and calls
`solar_os_ble_session_close(session)` on every exit path, including errors.
Close invalidates the handle immediately. An in-flight call pins the underlying
slot and its result until its caller returns, so closing and recreating a
session cannot transfer the old result to a new owner. Numeric handles and
connection/request tokens never wrap within a boot; exhaustion fails closed.
These are runtime handles, not persistent identifiers.

`solar_os_ble_peer_get_info()` reports a peer's connection snapshot, busy and
retirement state; `session_get_info()` reports the legacy default connection.
Each peer permits one blocking operation; different peers can operate concurrently.
Other tasks may cancel or close that session while its caller waits.
The service serializes lifecycle transitions and backend submission; metadata
locks are released before backend calls. Blocking waits do not hold the
submission lock.

## Cancellation and timeouts

`solar_os_ble_session_cancel()` keeps session/peer handles but aborts all its
connections and wakes waiting callers with `SOLAR_OS_BLE_ERR_CANCELLED`.
Cancellation cannot undo a write already transmitted. Closing a session has
the same cancellation behavior and also invalidates the handle.

`solar_os_ble_session_set_cancel_check()` installs an optional cooperative
cancellation check while the session is idle. A waiting connect/read/write
caller runs it outside service locks at intervals of at most 50 ms (subject to
task scheduling). The check must not block or raise interpreter exceptions.
The context must remain valid until the operation returns. A true result uses
the same cancellation/retirement path as explicit cancellation. Sessions without
a check retain the normal blocking wait. Reused slots clear the old check.

A connect/read/write timeout retires the connection and returns
`ESP_ERR_TIMEOUT`. A new operation cannot reuse the connection while an old
request might still complete. Reconnect returns `ESP_ERR_INVALID_STATE` while
retirement is pending. If teardown submission fails, the slot stays reserved;
cancel or a later connect attempt retries submission. A missing terminal
callback never makes the slot reusable merely because time elapsed.

Sleep cancels generic operations and invalidates their connection lifetime.
Session handles survive sleep, but apps must reconnect and rediscover
characteristics after resume. The legacy blocking discovery scan is rejected
while a generic connection is active or retiring, so it cannot delay
cancellation of that client's requests. Keyboard scan/reconnect policy remains
in the HID profile.

## Backend event contract

`solar_os_ble.c` owns session identity, connection ownership, operation
completion, timeouts, discovery cache, and per-session read-result buffers.
The private `solar_os_ble_backend.h` interface carries connection lifetime
(`epoch`) and request tokens. The service checks epoch, request, operation
type, connection ID, and characteristic handle before accepting a completion.
It ignores events from cancelled or previous lifetimes.

The NimBLE backend allocates a separate client entry per connection epoch.
`solar_os_ble_nimble.c` owns generic discovery, UUID conversion, and request
submission. Application tasks copy requests into adapter-owned storage and
enqueue work on the NimBLE host queue. They do not enter the host while holding
an adapter lock. GAP/discovery callbacks carry immutable connection epochs;
read/write callbacks carry immutable request tokens.

A failed connect has no live transport and retires immediately. Cancellation
before host submission does not cancel another profile's connection attempt.
Cancellation after submission cancels that attempt or terminates its link.
NimBLE aborts outstanding ATT procedures before delivering GAP disconnect;
disconnect is the retirement barrier. The adapter also rejects old tokens if
a transport handle is reused. The host must stop before callback storage is
deinitialized.

Internal service-event delivery is synchronous: the service copies borrowed
read bytes before returning. It never invokes app or interpreter callbacks.

`solar_os_ble_keyboard.c` retains boot policy, scan selection, one remembered
keyboard, layout, input translation, pairing UI, and reconnect scheduling. It
owns shared NimBLE initialization, bond storage and sleep/resume. Public
addresses remain in display order; conversion to NimBLE byte order occurs only
at the transport boundary.

`solar_os_ble_hid.c` is SolarOS's bounded HID-over-GATT client; it does not use
ESP-IDF's HID host transport. Its asynchronous setup authenticates/encrypts,
negotiates MTU, discovers HID/battery services, reads report maps/references,
subscribes to keyboard input and battery CCCDs, and reads the initial Battery
Level when the characteristic permits it. A bounded report-map
classifier identifies keyboard input IDs; the existing SolarOS key-report
decoder still interprets their payloads.

HID limits are five relevant HID/battery services, 64 characteristics per
service, 32 subscribed reports, a 2048-byte report map and 64-byte notification
payload. Report-map global nesting is limited to eight, collection nesting to
16; unsupported long items/local delimiters are rejected. Limits fail closed
instead of truncating discovery into an apparently ready keyboard.

Connection establishment is limited to three seconds, each discovery step to
ten seconds, passkey entry to 60 seconds, and the entire HID open to 90 seconds.
Sleep closes the admission gate before cancelling setup, so a racing worker
cannot start another connection. Workers are never forcibly deleted during
HID setup. Retirement waits for disconnect and delivery of the final policy
event. Input queue overflow disconnects the keyboard and releases pressed keys;
two queue slots are reserved for OPEN/CLOSE.

NimBLE and Bluedroid do not share bond storage. Existing keyboard preferences
and the remembered address remain, but keyboards previously bonded using
Bluedroid need to be forgotten/re-paired on both sides. Generic GATT databases
are rediscovered per connection, not persisted to NVS. Forget removes the
NimBLE bond synchronously and clears remembered state only after success or
confirmation that the bond is absent.

The firmware enables NimBLE central/observer roles plus peripheral/GATT-server
support so incoming ATT requests, including MTU exchange, have server handlers.
This does not start advertising automatically. Application-owned services and
connectable advertising are exposed under `solaros.ble.server`. Broadcaster
support remains disabled. Before ESP-IDF configuration, the build detects an
active generated `sdkconfig` or `sdkconfig.*` with Bluetooth enabled but missing
NimBLE, central/observer/peripheral roles, the GATT server, or dynamic service
support. It prints the missing settings, removes that file without a backup, and
regenerates it from the selected SDK configuration defaults. This resets local menuconfig
changes in that file. Defaults files are never removed. Repository defaults
enable NimBLE, peripheral support, and the GATT server, and disable Bluedroid.

## Existing GATT limits

- There is no fixed SolarOS session or peer-count limit. Allocation can fail with
  `ESP_ERR_NO_MEM`. `solar_os_ble_peer_capacity()` reports the total generic-peer
  budget, not the number currently free. Exhausting it returns
  `SOLAR_OS_BLE_ERR_CAPACITY` without disturbing existing connections.
- Configure `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` in the firmware's menuconfig.
  Classic ESP32 also needs a matching `CONFIG_BTDM_CTRL_BLE_MAX_CONN`; ESP32-S3
  uses `CONFIG_BT_CTRL_BLE_MAX_ACT`, which must leave room for scanning as well
  as connections. SolarOS uses the smaller effective budget and reserves one
  connection for the keyboard, even when it is absent. Defaults select three
  host connections (keyboard plus two generic peers); this is not a policy cap.
  Existing generated `sdkconfig.*` files must also be updated. Rebuild and flash
  after changing capacity; it is not a runtime setting.
- GAP connection establishment is serialized by the host. A concurrent connect
  may fail with `ESP_ERR_INVALID_STATE`; existing links remain intact. Connect
  peers sequentially, then operate on their independent connections.

- Read/write buffers remain limited to 128 bytes. Discovery is bounded at 24
  services and 64 characteristics per service; exceeding a limit fails setup.
- MTU exchange completes before service/characteristic discovery. Status reports
  the negotiated MTU; writes must fit in MTU minus three bytes. A peer refusing
  exchange can continue using the default MTU.
- Writes with response wait for GATT completion. Writes without response wait
  for local stack completion, which does not acknowledge remote application
  receipt.
- Characteristic handles are valid for the connection that discovered them.
  Apps must not retain them across reconnects.
- Python/Lua expose synchronous client operations under `solaros.ble.gatt`,
  with explicit peer handles, a lazily allocated runtime-owned session and
  automatic cleanup of every peer before VM teardown. Cooperative checks use
  the existing stop/deadline signals. Legacy
  `solaros.ble.read()` still reads decoded keyboard input.
  Peripheral operations use `solaros.ble.server`. Notifications and indications
  use the per-peer polling interface described below.
- `solaros.ble.hid` uses that same runtime-owned peripheral lease but publishes
  a native, fixed HID-over-GATT service. The OS owns encrypted characteristics,
  Report Reference descriptors, pairing/bonding, typed keyboard/mouse/gamepad
  report state, and neutral-report teardown. It accepts one remote host and
  exposes only copied status and events to Python or Lua.
- `service.ble` selects the service, adapter, and keyboard profile under the
  existing package and board capability gates.

## Notification and indication subscriptions

`solar_os_ble_peer_subscribe(session, peer, handle, mode, timeout_ms)` uses mode
0 to unsubscribe, 1 for notifications, or 2 for indications. The adapter checks
the characteristic properties, discovers descriptors from the value handle up
to the next characteristic declaration (or service end), and writes its actual
CCCD. It does not assume CCCD equals value handle plus one. Descriptor and write
callbacks carry immutable request tokens. Setup shares the peer's normal busy,
timeout, cancellation and retirement machinery; unrelated peers remain usable.
Only successful CCCD acknowledgement changes local delivery mode. A failed write
preserves the prior mode. Packets arriving before enable acknowledgement are
not delivered. Successful unsubscribe also purges queued values for its handle.

The service owns a dynamically allocated ring per peer. First subscribe defaults
to 16 entries; `solar_os_ble_peer_configure_queue()` allows a positive capacity
subject to checked allocation, while connected, idle and empty. Failed resizing
preserves storage and counters. No allocation occurs on incoming notification
callbacks. The adapter copies chained mbufs into a bounded local value, and the
synchronous service sink copies into its queue before the callback returns.
Queue-full drops the newest event; oversized or unreadable payloads are dropped
whole. Status exposes capacity, queued count and a saturating dropped counter.
`solar_os_ble_peer_poll()` is nonblocking, returns NOT_FOUND when empty, and can
run alongside a blocking operation. It returns owned copies, never host buffers.
Disconnect/cancel/timeout/sleep discard events and free the ring; peer loss
counters survive until the peer handle is released. NimBLE sends ATT indication
confirmations independently of application consumption and queue overflow.

Python and Lua expose subscribe/unsubscribe, configure_queue and poll through
the shared script descriptor. Poll returns handle, binary data and indication
flag (or None/nil when empty). No interpreter callback runs on the host task.

## Validation

Runtime service registration uses a project-local, version-checked NimBLE SDK
overlay. See [the overlay contract and tests](../patches/nimble/README.md).
Dynamic registration is enabled. The application server API is documented in
[the server contract](ble-server.md).

Host-driven BLE scripts and Python source checks live in the sibling
`solar_os_test` repository; see its `doc/ble.md` for the controlled BlueZ GATT
peer, explicit notification/indication sends, and opt-in live delivery test.
Compiled C tests remain here with the production source and host stubs.

Build and run the host tests:

```sh
make -C tests/host ble_service_test ble_nimble_test ble_hid_test ble_lua_bindings_test
make -C tests/host ble_multi_peer_test
tests/host/ble_multi_peer_test
tests/host/ble_service_test
tests/host/ble_nimble_test
tests/host/ble_hid_test
tests/host/ble_lua_bindings_test
```

The session tests use actual pthread waits and a controlled backend to exercise
cross-task close/cancel, owner isolation, timeout quarantine, stale handles and
events, reused transport IDs, sleep, and compatibility calls. Adapter and HID
tests execute the production state machines against controlled NimBLE callbacks,
including failed connection, cancellation before/after submission, stale epochs
and requests, discovery bounds, chained mbufs, binary writes, MTU limits,
CCCD subscription, report-map bounds and notification queue pressure.
Firmware builds compile against real IDF
headers. Cooperative checks are tested during connect, read, and write, including
callback reset when a slot is reused. The Lua test runs the actual interpreter
and bindings against the session service with a controlled radio backend. It
checks binary values, argument validation, ownership, VM cleanup, and stop during
a blocking read. Python has descriptor/source regression checks and firmware
build coverage; its runtime behavior also needs device validation.

Hardware checks should cover keyboard input/reconnect and sleep/wake, shell GATT
connect/discover/read/write, repeated disconnect/reconnect, and recovery after
a GATT timeout. Run discovery/read/write from both Python and Lua, stop a script
during a pending operation, and confirm the shell can subsequently reconnect
without rebooting. Host tests and successful builds do not establish those radio
and lifecycle results or live heap use.

Multi-peer hardware validation should keep the keyboard connected while one
application reads two peripherals. Disconnect or time out either peripheral and
confirm the other still works. Attempt a connection beyond configured capacity,
check that existing links survive, and measure heap use before connection and
after all peers are released. Repeat across script stop and sleep/wake.

Subscription checks should use two notifying peers alongside the keyboard:
verify notification/indication flags and binary payloads, subscription write
errors and timeout isolation, unsubscribe suppression, queue overflow counters,
and cleanup on VM exit and sleep. Host tests exercise these state transitions;
radio delivery and indication acknowledgement still require target validation.
