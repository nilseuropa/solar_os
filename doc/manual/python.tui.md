+++
id = "python.tui"
title = "Python text user-interface API"
section = "api"
summary = "Build terminal applications from MicroPython"
aliases = ["py.tui"]
keywords = "python py tui terminal text interface curses keyboard keys input box bold inverse"
agent_reference_sections = true
packages_any = ["app_python"]
+++
# Python text user-interface API

[API overview](python.md) · [Lua text user interfaces](lua.tui.md)

Use the shared layout on displays and cursor-addressable port shells:

```python
from solaros import tui
_, _, body, _, _, _ = tui.layout()
tui.title("Example")
tui.cell(body[0], 0, body[3], "Shared layout")
tui.help("Enter open  Esc exit")
```

## `solaros.tui`

TUI functions provide a small curses-like text UI layer over the SolarOS terminal. Drawing calls are queued onto the foreground UI side, so Python scripts do not write terminal memory directly.

Attributes:

- `NORMAL`
- `BOLD`
- `INVERSE`

Functions:

- `rows()`: return terminal rows.
- `cols()`: return terminal columns.
- `size()`: return `(rows, cols)`.
- `clear()`: clear the terminal.
- `refresh()`: atomically commit the buffered TUI frame and flush it.
- `move(row, col)`: move the terminal cursor.
- `write(text[, attr])`: write at the current cursor.
- `addstr(row, col, text[, attr])`: move and write text.
- `putch(row, col, ch[, attr])`: draw one character or codepoint.
- `hline(row, col, width[, attr])`: draw a horizontal line.
- `vline(row, col, height[, attr])`: draw a vertical line.
- `vrule(row, col, height[, width[, attr]])`: draw a continuous pixel vertical rule.
- `box(row, col, height, width[, attr])`: draw a box.
- `fill(row, col, height, width[, ch[, attr]])`: fill a rectangle.
- `getch([timeout_ms])`: return a key code or `None`. A positive timeout
  also presents pending TUI drawing before waiting.

Common key constants include `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`,
`KEY_CTRL_LEFT`, `KEY_CTRL_RIGHT`, `KEY_HOME`, `KEY_END`, `KEY_DELETE`,
`KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Example:

```python
import solaros
from solaros import tui

rows, cols = tui.size()
tui.clear()
tui.box(0, 0, rows, cols)
tui.addstr(1, 2, "SolarOS TUI", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit():
    key = tui.getch(250)
    if key == tui.KEY_ESCAPE:
        break
```

## Quick reference

High-level: layout, cell, title, help, tab, list_move, input_edit, input.
Rectangles are `(row, col, height, width)`. The low-level API remains.

`input(row, col, width, label, text, cursor, view[, attr[, masked]])` draws an
editable input row. Set `masked=True` to draw one `*` per UTF-8 character. The
mask is render-only: `text` and the value returned by `input_edit()` remain
unchanged, so a script must still avoid logging secrets and discard them when
they are no longer needed.

Use `tui.getch()` for keyboard characters and navigation keys. Use
`solaros.input.read()` for foreground touch, mouse, and joystick events.
