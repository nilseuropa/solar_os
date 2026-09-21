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

## Quick reference

Use `solaros.input`, `solaros.hid`, `solaros.clipboard` for input and clipboard.
See `man python` for runtime conventions and service availability.
