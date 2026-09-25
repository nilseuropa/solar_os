+++
id = "lua.gfx"
title = "Lua graphics API"
section = "api"
summary = "Draw through SolarOS displays from Lua"
aliases = []
keywords = "lua gfx graphics display screen draw pixel line rectangle circle font oled lcd framebuffer present"
agent_reference_sections = true
packages_any = ["app_lua"]
+++
# Lua graphics API

[API overview](lua.md) · [Python graphics](python.gfx.md)

`solaros.gfx` draws through the display owned by the current foreground
application. The module is already available as `solaros`; it can also be
loaded with `require("solaros")`.

Graphics ownership does not deliver input implicitly. Use
`solaros.input.read()` for touch coordinates, mouse deltas, and joystick axes.

## Draw safely

```lua
local solaros = require("solaros")
local gfx = solaros.gfx

gfx.begin()
local ok, err = pcall(function()
    local width = gfx.width()
    local height = gfx.height()
    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    gfx.fill_circle(math.floor(width / 2), math.floor(height / 2),
                    math.floor(math.min(width, height) / 4))
    gfx.present()
end)
gfx["end"]()
if not ok then error(err) end
```

Lua uses `gfx["end"]()` because `end` is a language keyword. The cleanup must
run even when drawing fails.

## Attached displays

A port shell has no current foreground display. Discover a ready target with
`display list` and pass its real name to `gfx.begin(name)`. Never assume that an
example such as `oled0` exists.

## Bitmaps and sprites

`gfx.bitmap(x, y, width, height, data)` draws packed 1-bit XBM data in the
current color. `gfx.sprite(...)` is an alias intended for transparent pixel-art
objects. Rows contain `(width + 7) // 8` bytes, least-significant bit first.
Set bits are drawn and clear bits leave the existing framebuffer unchanged.
Pass the data as a binary string of exactly the required size. One call accepts
at most 128 packed bytes.

```lua
local person = string.char(0x18, 0x3c, 0x18, 0x7e, 0x18, 0x24, 0x42, 0x00)
gfx.sprite(20, 20, 8, 8, person)
```

## Icons

`gfx.icon(x, y, name, size)` draws an Open Iconic symbol in the current color.
Names are lowercase and hyphenated, such as `folder`, `tablet`, and
`musical-note`. Size must be `8`, `16`, `32`, `48`, or `64` pixels.

```lua
gfx.icon(20, 20, "tablet", 32)
```

## Raster images

When the firmware includes `media.image`, `solaros.image` decodes static PNG,
JPEG, GIF, and WebP files into native PSRAM-backed handles. This keeps offline
map tiles and other large images outside the Lua heap. Up to 16 handles can be
open at once.

```lua
local image = solaros.image
local tile = image.open("/sdcard/maps/12/2200/1342.png")
local source_width, source_height = image.size(tile)
image.draw(tile, -24, 32)
image.draw(tile, 240, 32, 128, 128)
gfx.present()
image.close(tile)
```

`image.draw(handle, x, y)` uses the source size. Supplying both `width` and
`height` applies nearest-neighbor scaling. Coordinates can be negative and the
native renderer clips to the display. A queued draw keeps its own reference, so
closing or replacing a tile immediately after queuing it is safe.

## Colors

Use `gfx.WHITE`, `gfx.LIGHT`, `gfx.DARK`, `gfx.BLACK`, `gfx.gray(level)`, or
`gfx.rgb(red, green, blue)`. RGB components are `0..255`. On color TFTs, the
named colors and `gray(level)` span the `setterm foreground` and `background`
theme, while `rgb(...)` stays literal in the lazily allocated indexed canvas.
One-bit displays keep the existing luminance and dither path.

## `solaros.gfx`

- `solaros.gfx`: foreground graphics drawing functions

## Graphics

`solaros.gfx` draws through the foreground graphics service. `begin()` uses the
display framebuffer of the shell that launched the script; from a port or
headless shell it raises an error because there is no foreground display.
`begin(target)` claims a verified named display target, such as one returned by
`solaros.expansion.devices()`, until `end()` or script cleanup. Colors are
`WHITE`, `LIGHT`, `DARK`, `BLACK`, `gray(level)` with `0..GRAY_MAX`, and
`rgb(red, green, blue)` with `0..255` components. On color TFT targets, the
named colors and `gray(level)` span the `setterm foreground` and `background`
theme, while `rgb(...)` remains literal. One-bit targets keep the existing
luminance and ordered-dither path. Fonts
are `FONT_SMALL`, `FONT_MONO`, `FONT_BOLD`, regular document fonts
`FONT_MONO_12` through `FONT_MONO_20`, bold document fonts `FONT_BOLD_12`
through `FONT_BOLD_20`, and matching italic/bold-italic constants. Italic
constants currently map to the closest upright face in the trimmed firmware
font set.

Functions:

- `begin([target])`, `end()`
- `width()`, `height()`, `size()`
- `clear([color])`
- `gray(level)`
- `rgb(red, green, blue)`
- `color([color])`, `set_color(color)`
- `font([font])`, `set_font(font)`
- `pixel(x, y)`, `line(x0, y0, x1, y1)`
- `rect(x, y, width, height)`, `fill_rect(x, y, width, height)`
- `circle(x, y, radius)`, `fill_circle(x, y, radius)`
- `icon(x, y, name, size)` with a lowercase, hyphenated Open Iconic name such
  as `tablet`; size is `8`, `16`, `32`, `48`, or `64`
- `bitmap(x, y, width, height, data)`, `sprite(...)` alias
- `text(x, baseline_y, text)`
- `refresh()`, `present()`
- `getch([timeout_ms])`

When `media.image` is present, `solaros.image` provides `open(path)` and its
`load(path)` alias, `size(handle)`, `draw(handle, x, y[, width, height])`,
`close(handle)`, and `close_all()`. Runtime cleanup closes remaining handles.

Bitmap and sprite rows are packed least-significant bit first, with
`(width + 7) // 8` bytes per row. Set bits draw in the current color and clear
bits remain transparent. One call accepts at most 128 packed bytes, enough for
a 32 by 32 sprite. Lua passes the packed bytes in a binary string.

Example:

```lua
local solaros = require("solaros")
local gfx = solaros.gfx

gfx.begin()
local w, h = gfx.size()
gfx.clear(gfx.WHITE)
gfx.color(gfx.BLACK)
gfx.rect(8, 8, w - 16, h - 16)
gfx.font(gfx.FONT_BOLD)
gfx.text(24, 36, "SolarOS Lua")
gfx.color(gfx.gray(12))
gfx.fill_circle(w // 2, h // 2, 36)
gfx.color(gfx.BLACK)
gfx.circle(w // 2, h // 2, 36)
gfx.refresh()

while not solaros.should_exit() do
    local key = gfx.getch(250)
    if key == gfx.KEY_ESCAPE then
        break
    end
end

gfx["end"]()
```

For an attached auxiliary display, first verify its ready target name, then
pass that name:

```lua
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx["end"]()
```

## Quick reference

Lua: use the preloaded solaros table or local solaros = require("solaros"),
then assign local gfx = solaros.gfx. gfx.begin() uses the current foreground
display and errors from a port/headless shell where there is none. For an
attached display, the agent must call display_list and pass a returned ready
name; absent names raise ESP_ERR_NOT_FOUND. Use width, height or size; clear;
color; pixel, line, rect, fill_rect, circle, fill_circle, icon, text; refresh or
present. Use bitmap(x, y, width, height, data) or its sprite alias for
transparent packed 1-bit XBM data, with at most 128 bytes per call. Colors are
gfx.WHITE, gfx.LIGHT, gfx.DARK, gfx.BLACK, and
gfx.gray(level), and gfx.rgb(red, green, blue); pass these values to clear and color, never color-name
strings or guessed integers. Call gfx["end"]() because end is a Lua keyword.
Required attached-display pattern (replace the quoted target with a ready
display_list name):

```lua
local solaros = require("solaros")
local gfx = solaros.gfx
gfx.begin("verified-ready-target")
local ok, err = pcall(function()
    gfx.clear(gfx.WHITE)
    gfx.color(gfx.BLACK)
    -- draw here
    gfx.present()
end)
gfx["end"]()
if not ok then error(err) end
```
