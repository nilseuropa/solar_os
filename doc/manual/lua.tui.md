+++
id = "lua.tui"
title = "Lua text user-interface API"
section = "api"
summary = "Build terminal applications from Lua"
aliases = []
keywords = "lua tui terminal text interface curses keyboard keys input box bold inverse"
packages_any = ["app_lua"]
+++
# Lua text user-interface API

Use the shared layout on displays and cursor-addressable port shells:

```lua
local tui = solaros.tui
local _, _, body = table.unpack(tui.layout())
tui.title("Example")
tui.cell(body[1], 0, body[4], "Shared layout")
tui.help("Enter open  Esc exit")
```

## Quick reference

High-level: layout, cell, title, help, tab, list_move, input_edit, input.
Rectangles are `{row, col, height, width}`. The low-level API remains.

`input(row, col, width, label, text, cursor, view[, attr[, masked]])` draws an
editable input row. Set `masked` to `true` to draw one `*` per UTF-8 character.
The mask is render-only: `text` and the value returned by `input_edit()` remain
unchanged, so a script must still avoid logging secrets and discard them when
they are no longer needed.

Use `tui.getch()` for keyboard characters and navigation keys. Use
`solaros.input.read()` for foreground touch, mouse, and joystick events.
