# SolarOS Lua API

SolarOS embeds Lua as the `lua` foreground application. It can run an interactive REPL or execute `.lua` files from SD card.

The SolarOS API is preloaded as the global table `solaros`. A minimal `require("solaros")` shim is also provided:

```lua
local solaros = require("solaros")

print("SolarOS " .. solaros.version())
print(solaros.identity.format())
```

Lua allocations prefer PSRAM. Host-facing Lua `io`, `os`, and dynamic package loading are intentionally not opened; scripts should use SolarOS services for hardware, storage, networking, and foreground UI.

## Top-Level Helpers

- `solaros.write(text)`: write to the foreground terminal.
- `solaros.version()`: return the firmware version.
- `solaros.should_exit()`: return whether the foreground app was asked to exit.
- `solaros.battery_status()`: short battery status table or `nil`.
- `solaros.wifi_status()`: short Wi-Fi status table.
- `solaros.environment()`: temperature and humidity table or `nil`.

## Service Tables

Lua mirrors the Python `solaros` module structure:

- `solaros.storage`: `status`, `is_mounted`, `mount`, `unmount`, `mount_point`, `usage`, `resolve`, `rescan`, `blocks`, `block_count`, `block`, `usage_for_block`, `mkdir`, `rmdir`, `remove`, `rename`, `copy`, `mount_volume`, `unmount_volume`
- `solaros.time`: `uptime_ms`, `uptime`, `datetime`, `utc_datetime`, `set_datetime`, `set_utc_datetime`, `utc_to_local`, `local_to_utc`, `is_valid`, `timezone`, `set_timezone`, `ntp_sync`
- `solaros.battery`: `status`
- `solaros.sensors`: `environment`
- `solaros.wifi`: `status`, `status_text`, `start`, `stop`, `connect`, `connect_saved`, `disconnect`, `forget`, `forget_ssid`, `forget_all`, `known`, `scan`, `ap_start`, `ap_stop`, `nat`
- `solaros.mqtt`: `status`, `connect`, `disconnect`, `publish`, `subscribe`, `read` when the `net` package is compiled
- `solaros.gpio`: constants `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`; functions `pins`, `allowed`, `mode`, `configure`, `read`, `write`
- `solaros.adc`: `pins`, `read`
- `solaros.pwm`: constants `FREQ_MIN`, `FREQ_MAX`; functions `status`, `set`, `off`
- `solaros.i2c`: `info`, `probe`, `scan`, `read_reg`, `write_reg`
- `solaros.uart`: `status`, `baud`, `is_valid_baud`, `mode`, `write`, `read`
- `solaros.audio`: `status`, `deinit`, `off`, `set_volume`, `set_mic_gain`, `tone`, `level`, `loopback`, `wav_info`, `record_wav`, `play_wav`
- `solaros.ble`: `status`, `connected`, `pair`, `forget`, `layout`, `read`
- `solaros.clipboard`: `set`, `get`, `size`, `clear`
- `solaros.identity`: `user`, `hostname`, `format`
- `solaros.net`: `ping` when the `net` package is compiled
- `solaros.ssh_keys`: `default_paths`, `default_exists`, `status`, `generate`, `remove` when the `net` package is compiled
- `solaros.jobs`: `list`, `count`, `status`, `start`, `stop`
- `solaros.apps`: `list`, `find`
- `solaros.tui`: curses-like terminal drawing functions
- `solaros.gfx`: foreground graphics drawing functions

Lua strings are binary-safe, so byte-oriented APIs such as `uart.read`, `i2c.read_reg`, `clipboard.get`, and `mqtt.read().payload` return Lua strings.

`solaros.uart.status()` includes `rx_buffered` and `rx_buffered_valid`. When another owner is actively using the UART, `rx_buffered_valid` is `false` because the live RX count is not sampled.

## TUI

`solaros.tui` draws through the foreground UI queue. It exposes constants `NORMAL`, `BOLD`, `INVERSE`, plus common key constants such as `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT`, `KEY_ESCAPE`, `KEY_PAGE_UP`, and `KEY_PAGE_DOWN`.

Functions:

- `rows()`, `cols()`, `size()`
- `clear()`, `refresh()`
- `move(row, col)`, `write(text[, attr])`, `addstr(row, col, text[, attr])`
- `putch(row, col, ch[, attr])`
- `hline(row, col, width[, attr])`, `vline(row, col, height[, attr])`, `vrule(row, col, height[, width[, attr]])`
- `box(row, col, height, width[, attr])`
- `fill(row, col, height, width[, ch[, attr]])`
- `getch([timeout_ms])`

Example:

```lua
local solaros = require("solaros")
local tui = solaros.tui

tui.clear()
tui.box(0, 0, tui.rows(), tui.cols())
tui.addstr(1, 2, "SolarOS Lua", tui.BOLD)
tui.addstr(3, 2, "Press ESC")
tui.refresh()

while not solaros.should_exit() do
    local key = tui.getch(250)
    if key == tui.KEY_ESCAPE then
        break
    end
end
```

## Graphics

`solaros.gfx` draws through the foreground graphics service. Colors are `WHITE`, `LIGHT`, `DARK`, `BLACK`, and `gray(level)` with `0..GRAY_MAX`. Fonts are `FONT_SMALL`, `FONT_MONO`, `FONT_BOLD`, regular document fonts `FONT_MONO_12` through `FONT_MONO_20`, bold document fonts `FONT_BOLD_12` through `FONT_BOLD_20`, and matching italic/bold-italic constants. Italic constants currently map to the closest upright face in the trimmed firmware font set.

Functions:

- `begin()`, `end()`
- `width()`, `height()`, `size()`
- `clear([color])`
- `gray(level)`
- `color([color])`, `set_color(color)`
- `font([font])`, `set_font(font)`
- `pixel(x, y)`, `line(x0, y0, x1, y1)`
- `rect(x, y, width, height)`, `fill_rect(x, y, width, height)`
- `circle(x, y, radius)`, `fill_circle(x, y, radius)`
- `text(x, baseline_y, text)`
- `refresh()`, `present()`
- `getch([timeout_ms])`

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

## Notes

Lua tables returned as lists use normal Lua 1-based array indexes. Direct block lookup with `solaros.storage.block(index)` follows the underlying storage service index, matching Python's 0-based `block(index)`.

The Lua bridge intentionally does not expose raw SSH/SCP session handles. Those need explicit object lifetime and event-loop rules before becoming scriptable.
