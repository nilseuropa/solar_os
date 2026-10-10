+++
id = "python.input"
title = "Python input and clipboard API"
section = "api"
summary = "Input and clipboard: input, hid, clipboard"
keywords = "python solaros api input gesture pointer axis hid clipboard"
packages_any = ["app_python"]
agent_reference_sections = true
+++
# Python input and clipboard API

[API overview](python.md) · [Lua input and clipboard](lua.input.md)

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

## `solaros.input`

When `service.input-keymap` is included,
`solaros.input.load_keymap(name, path)` loads a validated JSON mapping file for
the named attachment, and `solaros.input.reset_keymap(name)` restores its
built-in map. Both return `None`; failures raise an exception. Applying a map
releases held keys. Mappings last until detach or reboot; a startup script
can reload a saved file. `solaros.input.keymap_info(name)` reports whether a source supports mapping,
physical keys, modifiers, layers, and tap/hold, together with key count and
optional matrix metadata. Other drivers currently report `supported=false`.
See [source keymaps](input.keymap.md) for fields and mapping formats.

Foreground scripts can receive generic pointer, axis, and gesture events routed to
their active session. `sources()` lists registered input sources with `source`,
`name`, `source_class`, `source_class_name`, `capabilities`, and `ready`.

- `read([timeout_ms])`: return the next pointer, axis, or gesture event dictionary, or
  `None`. The maximum timeout is 60000 ms.
- `clear()`: discard queued pointer, axis, and gesture events and return the number
  discarded.
- `status()`: return `available`, `queued`, `capacity`, and cumulative
  `dropped` counters.

Pointer dictionaries have `type="pointer"`, source metadata, `pointer_id`,
numeric and named `mode`/`action`, `x`, `y`, `delta_x`, `delta_y`, `buttons`,
and `target`. Touch and other absolute sources use `x`/`y`; relative mice use
the deltas. Axis dictionaries have `type="axis"`, source metadata, numeric and
named `axis`, `value`, and `delta`. Gesture dictionaries have `type="gesture"`,
numeric and named `gesture` and `direction`, `flags`, a source-specific `value`,
and the original sensor `raw` word. AirWheel values are signed counter steps;
32 steps approximate one revolution.

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
    elif event["type"] == "axis":
        print("axis", event["axis_name"], event["value"], event["delta"])
    else:
        print("gesture", event["gesture_name"], event["direction_name"])
```

The queue holds 16 events. When it is full, the oldest event is discarded so
the script receives current pointer state; inspect `status()["dropped"]` when
loss matters. Event reads are available only to a foreground Python app. Agent
or other headless source runners report `available=False` and return `None`.
Keyboard characters and navigation keys remain available through
`solaros.tui.getch()`.

### Local keyboard capture

Foreground scripts can exclusively capture one ready named key-event source:

- `capture_keyboard(name)`: claim that source before SolarOS shortcuts and
  character decoding. One Python or Lua app can own a capture at a time.
- `read_key()`: nonblocking; return the next captured key dictionary or
  `None`. A key dictionary has `type="key"`, source metadata,
  `physical_key`, canonical HID `usage`, translated `key`, `modifiers`,
  and `action`. A zero usage means the source cannot identify that HID key.
- `release_keyboard()`: release this runtime's capture and discard its queue.

Actions are `KEY_PRESS`, `KEY_RELEASE`, and `KEY_REPEAT`. Modifier bits
follow USB HID order (left Ctrl, Shift, Alt, GUI, then their right counterparts);
`MOD_CTRL` and `MOD_ALT` match either side.

Captured input bypasses local focus switching and the usual app-exit key.
The script must implement its own exit chord and check `should_exit()`.
Capture applies to the named local source even when another session has input
focus. Port characters and other sources keep their normal routing.
Headless runners cannot claim a keyboard. Runtime exit and forced stop release
the claim automatically; use `try/finally` for normal cleanup.

The capture queue holds 32 events. Overflow discards queued events and delivers
`{"type": "reset"}` before subsequent keys. Source release, readiness loss,
and detach also deliver a reset. Release any forwarded held keys on reset;
detach ends capture of that source, including if its source ID is reused.

## Quick reference

Use `solaros.input`, `solaros.hid`, `solaros.clipboard` for input and clipboard.
See `man python` for runtime conventions and service availability.
