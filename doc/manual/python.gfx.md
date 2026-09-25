+++
id = "python.gfx"
title = "Python graphics API"
section = "api"
summary = "Draw through SolarOS displays from MicroPython"
aliases = ["py.gfx"]
keywords = "python py gfx graphics display screen draw pixel line rectangle circle font oled lcd framebuffer present"
agent_reference_sections = true
packages_any = ["app_python"]
+++
# Python graphics API

[API overview](python.md) · [Lua graphics](lua.gfx.md)

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

Use `gfx.WHITE`, `gfx.LIGHT`, `gfx.DARK`, `gfx.BLACK`, `gfx.gray(level)`, or
`gfx.rgb(red, green, blue)`. RGB components are `0..255`. On color TFTs, the
named colors and `gray(level)` span the `setterm foreground` and `background`
theme, while `rgb(...)` stays literal in the lazily allocated indexed canvas.
One-bit targets keep the existing luminance and dither path. Do not use
color-name strings or guessed integer values. Read dimensions with
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

## Raster images

When the firmware includes `media.image`, `solaros.image` decodes static PNG,
JPEG, GIF, and WebP files into native PSRAM-backed handles. This keeps map tiles
and other large images outside the MicroPython heap. Up to 16 handles can be
open at once.

```python
from solaros import gfx, image

tile = image.open("/sdcard/maps/12/2200/1342.png")
try:
    source_width, source_height = image.size(tile)
    image.draw(tile, -24, 32)
    image.draw(tile, 240, 32, 128, 128)
    gfx.present()
finally:
    image.close(tile)
```

`image.draw(handle, x, y)` uses the source size. Supplying both `width` and
`height` applies nearest-neighbor scaling. Coordinates can be negative and the
native renderer clips to the display. Drawing is queued with the other graphics
operations. A queued draw keeps its own reference, so it is safe to close or
replace a tile immediately after queuing it. `image.close_all()` releases every
handle owned by the current runtime; interpreter cleanup also does this.

## Icons

`gfx.icon(x, y, name, size)` draws an Open Iconic symbol in the current color.
Names are lowercase and hyphenated, such as `folder`, `tablet`, and
`musical-note`. Size must be `8`, `16`, `32`, `48`, or `64` pixels.

```python
gfx.icon(20, 20, "tablet", 32)
```

## `solaros.gfx`

Graphics functions provide queued access to the SolarOS foreground graphics
service. Call `begin()` before drawing and `refresh()`/`present()` to push the
frame to the display. With no argument, `begin()` uses the display framebuffer
of the shell that launched the script. A port or headless shell has no such
framebuffer, so targetless `begin()` raises `RuntimeError` instead of silently
drawing nowhere. `begin(target)` claims a verified named display target, such
as one returned by `solaros.expansion.devices()`, until `end()` or script
cleanup.

Colors:

- `WHITE`
- `LIGHT`
- `DARK`
- `BLACK`
- `GRAY_MAX`: maximum grayscale level accepted by `gray(level)`, currently `16`.

`gray(level)` returns a semantic shade from the `setterm foreground` color at
level `0` to the `setterm background` color at `GRAY_MAX`; `BLACK`, `DARK`,
`LIGHT`, and `WHITE` use the same theme range. `rgb(red, green, blue)` returns
an explicit RGB color from three `0..255` components. Color-capable TFT targets
preserve explicit RGB values in an indexed-color canvas. One-bit targets keep
the existing luminance and ordered-dither path.

Fonts:

- `FONT_SMALL`
- `FONT_MONO`
- `FONT_BOLD`
- `FONT_MONO_12`, `FONT_MONO_14`, `FONT_MONO_16`, `FONT_MONO_18`, `FONT_MONO_20`
- `FONT_BOLD_12`, `FONT_BOLD_14`, `FONT_BOLD_16`, `FONT_BOLD_18`, `FONT_BOLD_20`
- `FONT_ITALIC_12`, `FONT_ITALIC_14`, `FONT_ITALIC_16`, `FONT_ITALIC_18`, `FONT_ITALIC_20`
- `FONT_BOLD_ITALIC_12`, `FONT_BOLD_ITALIC_14`, `FONT_BOLD_ITALIC_16`, `FONT_BOLD_ITALIC_18`, `FONT_BOLD_ITALIC_20`

Italic constants currently map to the closest upright face in the trimmed firmware font set.

Functions:

- `begin([target])`: enter foreground graphics mode; without a target, require
  the current shell to have a display framebuffer; when `target` is provided,
  claim and draw to that named display target.
- `end()`: leave graphics mode and redraw the terminal.
- `width()`: return graphics width in pixels.
- `height()`: return graphics height in pixels.
- `size()`: return `(width, height)`.
- `clear([color])`: clear the graphics buffer, defaulting to `WHITE`.
- `gray(level)`: return a grayscale color value from `0` to `GRAY_MAX`.
- `rgb(red, green, blue)`: return an RGB color; each component is `0..255`.
- `color([color])`: get or set current drawing color.
- `set_color(color)`: alias for `color(color)`.
- `font([font])`: get or set current text font.
- `set_font(font)`: alias for `font(font)`.
- `pixel(x, y)`: draw one pixel.
- `line(x0, y0, x1, y1)`: draw a line.
- `rect(x, y, width, height)`: draw a rectangle outline.
- `fill_rect(x, y, width, height)`: draw a filled rectangle.
- `circle(x, y, radius)`: draw a circle outline.
- `fill_circle(x, y, radius)`: draw a filled circle.
- `icon(x, y, name, size)`: draw a named Open Iconic symbol; use a lowercase,
  hyphenated name such as `tablet`; size is `8`, `16`, `32`, `48`, or `64`.
- `bitmap(x, y, width, height, data)`: draw a transparent packed 1-bit XBM.
- `sprite(x, y, width, height, data)`: alias for `bitmap()`.
- `text(x, baseline_y, text)`: draw UTF-8 text.
- `refresh()`: present the graphics buffer.
- `present()`: alias for `refresh()`.
- `getch([timeout_ms])`: return a key code or `None`.

When `media.image` is present, `solaros.image` provides:

- `open(path)`, `load(path)`: decode a static raster and return a handle.
- `size(handle)`: return `(width, height)`.
- `draw(handle, x, y[, width, height])`: queue a clipped raster draw.
- `close(handle)`: close one handle.
- `close_all()`: close all handles owned by this Python runtime.

Bitmap and sprite rows are packed least-significant bit first, with
`(width + 7) // 8` bytes per row. Set bits draw in the current color and clear
bits remain transparent. One call accepts at most 128 packed bytes, enough for
a 32 by 32 sprite.

Example:

```python
import solaros
from solaros import gfx

gfx.begin()
w, h = gfx.size()
gfx.clear(gfx.WHITE)
gfx.color(gfx.BLACK)
gfx.rect(8, 8, w - 16, h - 16)
gfx.font(gfx.FONT_BOLD)
gfx.text(24, 36, "SolarOS Graphics")
gfx.color(gfx.gray(12))
gfx.fill_circle(w // 2, h // 2, 36)
gfx.color(gfx.BLACK)
gfx.circle(w // 2, h // 2, 36)
gfx.refresh()

while not solaros.should_exit():
    key = gfx.getch(250)
    if key == gfx.KEY_ESCAPE:
        break

gfx.end()
```

For an attached auxiliary display, first verify its ready target name with
`solaros.expansion.devices()`, then pass that name:

```python
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx.end()
```

## Quick reference

Python: import solaros; from solaros import gfx. gfx.begin() uses the current
foreground display and raises RuntimeError from a port/headless shell where
there is none. For an attached display, the agent must call display_list and
pass a returned ready name to gfx.begin(name); scripts can verify names with
solaros.expansion.devices(). An absent name raises ESP_ERR_NOT_FOUND. Use
width(), height(), or size(); clear(color); color(color); pixel, line, rect,
fill_rect, circle, fill_circle, icon, text; refresh() or present(); then end().
Use bitmap(x, y, width, height, data) or its sprite alias for transparent
packed 1-bit XBM data, with at most 128 bytes per call.
Standard min() and max() are available. Colors are gfx.WHITE, gfx.LIGHT,
gfx.DARK, gfx.BLACK, gfx.gray(level), and gfx.rgb(red, green, blue); pass these values to clear() and
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
