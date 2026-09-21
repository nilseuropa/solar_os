+++
id = "lua.input"
title = "Lua input and clipboard API"
section = "api"
summary = "Input and clipboard: input, hid, clipboard"
keywords = "lua solaros api input input hid clipboard"
packages_any = ["app_lua"]
agent_reference_sections = true
+++
# Lua input and clipboard API

[API overview](lua.md) · [Python input and clipboard](python.input.md)

## `solaros.hid`

- `solaros.hid`: typed `keyboard`, `mouse`, and `gamepad` tables when `service.hid` is compiled

## `solaros.clipboard`

- `solaros.clipboard`: `set`, `get`, `size`, `clear`

## `solaros.input`

- `solaros.input`: `sources`, `read`, `clear`, `status` for foreground pointer, axis, and gesture events

## Generic pointer and axis input

`solaros.input.sources()` lists registered input sources with their numeric
source, name, class, class name, capability bits, and ready state.
`read([timeout_ms])` returns the next pointer, axis, or gesture event table, or `nil`; the
maximum timeout is 60000 ms. `clear()` discards queued events, and `status()`
reports `available`, `queued`, `capacity`, and cumulative `dropped` counts.

Pointer events contain source metadata, `pointer_id`, numeric and named
`mode`/`action`, `x`, `y`, `delta_x`, `delta_y`, `buttons`, and `target`.
Absolute touch sources use the coordinates; relative mice use the deltas. Axis
events contain source metadata, numeric and named `axis`, `value`, and `delta`.
Gesture events contain numeric and named `gesture` and `direction`, `flags`, a
source-specific `value`, and the original sensor `raw` word. AirWheel values
are signed counter steps; 32 steps approximate one revolution.

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
    elseif event and event.type == "axis" then
        print("axis", event.axis_name, event.value, event.delta)
    elseif event then
        print("gesture", event.gesture_name, event.direction_name)
    end
end
```

The foreground queue holds 16 events and discards the oldest event when full.
Agent and other headless source runners report `available=false` and return
`nil`. Keyboard characters and navigation keys remain available through
`solaros.tui.getch()`.

## USB HID

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

## Quick reference

Use `solaros.input`, `solaros.hid`, `solaros.clipboard` for input and clipboard.
See `man lua` for runtime conventions and service availability.
