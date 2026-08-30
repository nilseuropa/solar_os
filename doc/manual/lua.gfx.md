+++
id = "lua.gfx"
title = "Lua graphics API"
section = "api"
summary = "Draw through SolarOS displays from Lua"
aliases = []
keywords = "lua gfx graphics display screen draw pixel line rectangle circle font oled lcd framebuffer present"
packages_any = ["app_lua"]
+++
# Lua graphics API

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

## Colors

Use `gfx.WHITE`, `gfx.LIGHT`, `gfx.DARK`, `gfx.BLACK`, `gfx.gray(level)`, or
`gfx.rgb(red, green, blue)`. RGB components are `0..255`. Color TFTs retain the
color in a lazily allocated indexed canvas; one-bit displays dither its
luminance.

## Quick reference

Lua: use the preloaded solaros table or local solaros = require("solaros"),
then assign local gfx = solaros.gfx. gfx.begin() uses the current foreground
display and errors from a port/headless shell where there is none. For an
attached display, the agent must call display_list and pass a returned ready
name; absent names raise ESP_ERR_NOT_FOUND. Use width, height or size; clear;
color; pixel, line, rect, fill_rect, circle, fill_circle, text; refresh or
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
