+++
id = "python.gfx"
title = "Python graphics API"
section = "api"
summary = "Draw through SolarOS displays from MicroPython"
aliases = ["py.gfx"]
keywords = "python py gfx graphics display screen draw pixel line rectangle circle font oled lcd framebuffer present"
packages_any = ["app_python"]
+++
# Python graphics API

`solaros.gfx` draws through the display owned by the current foreground
application. A script started from a display shell can use that display without
naming it. A script started from a port shell must use a ready attached display
name.

Graphics ownership does not deliver input implicitly. Use
`solaros.input.read()` for touch coordinates, mouse deltas, and joystick axes.

## Draw on the current display

```python
import solaros
from solaros import gfx

gfx.begin()
try:
    width = gfx.width()
    height = gfx.height()
    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    gfx.fill_circle(width // 2, height // 2, min(width, height) // 4)
    gfx.present()
finally:
    gfx.end()
```

Always put `gfx.end()` in `finally` so an exception releases the display.

## Draw on an attached display

First run `display list` or inspect `solaros.expansion.devices()`. Pass only a
ready target returned by discovery:

```python
gfx.begin("oled0")
```

An absent name raises `ESP_ERR_NOT_FOUND`. Calling `gfx.begin()` without a name
from a port or headless shell raises `RuntimeError` because that session has no
foreground display.

## Colors and dimensions

Use `gfx.WHITE`, `gfx.LIGHT`, `gfx.DARK`, `gfx.BLACK`, or `gfx.gray(level)`.
Do not use color-name strings or guessed integer values. Read dimensions with
`width()`, `height()`, or `size()` rather than assuming a panel size.

## Bitmaps and sprites

`gfx.bitmap(x, y, width, height, data)` draws packed 1-bit XBM data in the
current color. `gfx.sprite(...)` is an alias intended for transparent pixel-art
objects. Rows contain `(width + 7) // 8` bytes, least-significant bit first.
Set bits are drawn and clear bits leave the existing framebuffer unchanged.
The data must be a bytes-like object of exactly the required size, with a
maximum of 128 packed bytes per call.

```python
person = bytes((0x18, 0x3C, 0x18, 0x7E, 0x18, 0x24, 0x42, 0x00))
gfx.sprite(20, 20, 8, 8, person)
```

## Quick reference

Python: import solaros; from solaros import gfx. gfx.begin() uses the current
foreground display and raises RuntimeError from a port/headless shell where
there is none. For an attached display, the agent must call display_list and
pass a returned ready name to gfx.begin(name); scripts can verify names with
solaros.expansion.devices(). An absent name raises ESP_ERR_NOT_FOUND. Use
width(), height(), or size(); clear(color); color(color); pixel, line, rect,
fill_rect, circle, fill_circle, text; refresh() or present(); then end().
Use bitmap(x, y, width, height, data) or its sprite alias for transparent
packed 1-bit XBM data, with at most 128 bytes per call.
Standard min() and max() are available. Colors are gfx.WHITE, gfx.LIGHT,
gfx.DARK, gfx.BLACK, and gfx.gray(level); pass these constants to clear() and
color(), never color-name strings or guessed integers. Required
attached-display pattern (replace the quoted target with a ready display_list
name):

```python
import solaros
from solaros import gfx
gfx.begin("verified-ready-target")
try:
    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    # draw here
    gfx.present()
finally:
    gfx.end()
```
