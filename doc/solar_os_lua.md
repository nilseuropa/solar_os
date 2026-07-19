# SolarOS Lua API

SolarOS embeds Lua as the `lua` foreground application. It can run an interactive REPL or execute `.lua` files from storage.

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
- `solaros.battery_status()`: short battery status table or `nil` when battery support is compiled.
- `solaros.wifi_status()`: short Wi-Fi status table when Wi-Fi support is compiled.
- `solaros.environment()`: temperature and humidity table or `nil` when environmental sensor support is compiled.

## Service Tables

Lua mirrors the Python `solaros` module structure:

The Lua runtime package requires PSRAM. Hardware and network tables are present
only when the board/flavor includes the corresponding service package. For
example, an ODROID-GO full build includes Lua with `solaros.spi` and
`solaros.onewire`, while omitting `solaros.adc` and `solaros.i2c` because those
service packages are not available on that board.

- `solaros.storage`: `status`, `is_mounted`, `mount`, `unmount`, `mount_point`, `usage`, `resolve`, `rescan`, `blocks`, `block_count`, `block`, `usage_for_block`, `mkdir`, `rmdir`, `remove`, `rename`, `copy`, `mount_volume`, `unmount_volume`
- `solaros.time`: `uptime_ms`, `uptime`, `datetime`, `utc_datetime`, `set_datetime`, `set_utc_datetime`, `utc_to_local`, `local_to_utc`, `is_valid`, `timezone`, `set_timezone`, `ntp_sync`
- `solaros.battery`: `status` when battery support is compiled
- `solaros.sensors`: `environment` when environmental sensor support is compiled
- `solaros.wifi`: `status`, `status_text`, `start`, `stop`, `connect`, `connect_saved`, `disconnect`, `forget`, `forget_ssid`, `forget_all`, `known`, `scan`, `ap_start`, `ap_stop`, `nat` when Wi-Fi support is compiled
- `solaros.mqtt`: `status`, `connect`, `disconnect`, `publish`, `subscribe`, `read` when the `net` package is compiled
- `solaros.gpio`: constants `INPUT`, `OUTPUT`, `PULL_NONE`, `PULL_UP`, `PULL_DOWN`; functions `pins`, `allowed`, `mode`, `configure`, `read`, `write` when GPIO support is compiled. Pin tables include `expansion`, `allowed`, and `policy` (`free`, `releasable`, or `fixed`).
- `solaros.onewire`: `allowed`, `reset`, `scan`, `xfer` for the direct-pin compatibility API when OneWire support is compiled
- `solaros.led`: `status`, `set`, `on`, `off`, `toggle` when GPIO support is compiled
- `solaros.adc`: `pins`, `read` when ADC support is compiled
- `solaros.pwm`: constants `FREQ_MIN`, `FREQ_MAX`; functions `status`, `set`, `off` when PWM support is compiled
- `solaros.buses`: constants `MODE0` through `MODE3`, `SPI2_HOST`, `SPI3_HOST`, `DEFAULT_SPEED`, `MAX_SPEED`; functions `list`, `get`, `create_spi`, `remove`, `spi_xfer`, `spi_read`, `spi_write` when the resource service is compiled; `create_i2c`, `i2c_probe`, `i2c_scan`, `i2c_read_reg`, and `i2c_write_reg` are additionally present when I2C support is compiled; `create_onewire`, `onewire_reset`, `onewire_scan`, and `onewire_xfer` are additionally present when OneWire support is compiled; `create_uart`, `uart_write`, and `uart_read` are additionally present when UART support is compiled
- `solaros.expansion`: `drivers`, `devices`, `attach`, `detach` when the expansion service is compiled
- `solaros.i2c`: `info`, `probe`, `scan`, `read_reg`, `write_reg` when I2C support is compiled
- `solaros.spi`: constants `MODE0` through `MODE3`, `DEFAULT_SPEED`, and `MAX_SPEED`; functions `status`, `xfer`, `read`, `write` when SPI support is compiled
- `solaros.uart`: `status`, `baud`, `is_valid_baud`, `mode`, `write`, `read` when UART support is compiled
- `solaros.audio`: `status`, `deinit`, `off`, `set_volume`, `set_mic_gain`, `tone`, `level`, `loopback`, `wav_info`, `record_wav`, `play_wav` when audio support is compiled
- `solaros.ble`: `status`, `connected`, `pair`, `forget`, `layout`, `read` when BLE support is compiled
- `solaros.clipboard`: `set`, `get`, `size`, `clear`
- `solaros.identity`: `user`, `hostname`, `format`
- `solaros.net`: `ping` when the `net` package is compiled
- `solaros.ssh_keys`: `default_paths`, `default_exists`, `status`, `generate`, `remove` when the `net` package is compiled
- `solaros.jobs`: `list`, `count`, `status`, `start`, `stop`
- `solaros.sessions`: `create_shell`, `close`
- `solaros.apps`: `list`, `find`
- `solaros.tui`: curses-like terminal drawing functions
- `solaros.gfx`: foreground graphics drawing functions

Lua strings are binary-safe, so byte-oriented APIs such as `uart.read`, `i2c.read_reg`, `clipboard.get`, and `mqtt.read().payload` return Lua strings.

`solaros.onewire.scan(pin)` returns tables containing a 16-digit hexadecimal
`address` and numeric `family` code. `solaros.onewire.xfer(pin, read_len[, data])`
resets the bus, writes the binary-safe `data` string, and returns `read_len`
bytes. Reads and writes are each limited to 64 bytes.

## Named buses and expansion devices

`solaros.buses` discovers board-defined and runtime-created buses independently
of the legacy single-board-bus and direct-pin service tables.

- `list()` returns every bus table.
- `get(name)` returns one bus table.
- `create_i2c(name, config)` creates a runtime I2C bus and returns its table.
- `create_onewire(name, config)` creates a runtime 1-Wire bus and returns its table.
- `create_spi(name, config)` creates a runtime SPI bus and returns its table.
- `create_uart(name, config)` creates a lazy runtime UART bus and returns its table.
- `remove(name)` removes an idle runtime bus.
- `i2c_probe(bus, address)`, `i2c_scan(bus)`,
  `i2c_read_reg(bus, address, reg, length)`, and
  `i2c_write_reg(bus, address, reg, data)` operate on a selected named I2C bus
  when both the resource and I2C services are compiled.
- `onewire_reset(bus)`, `onewire_scan(bus)`, and
  `onewire_xfer(bus, read_len[, data])` operate on a selected registered
  OneWire bus when both the resource and OneWire services are compiled.
- `uart_write(bus, data)` and `uart_read(bus[, length[, timeout_ms]])` operate
  on a selected named UART when both the resource and UART services are compiled.
- `spi_xfer(bus, cs, data[, mode[, speed_hz]])`,
  `spi_read(bus, cs, length[, fill[, mode[, speed_hz]]])`, and
  `spi_write(bus, cs, data[, mode[, speed_hz]])` transfer on a selected named
  bus. Each raw transfer takes and releases a temporary lease automatically.

Bus tables contain `id`, `name`, `protocol`, `origin`, `sharing`, `ready`, and
`lease_count`, plus protocol-specific pins and configuration. `create_spi`
requires `host`, `sclk`, `mosi`, and a one-to-four-element `cs` array. `miso`
and `max_transfer_size` are optional. I2C bus tables include `port`, `sda_pin`,
`scl_pin`, and `speed_hz`. Named I2C operations take and release a shared lease
automatically; the legacy `solaros.i2c` table remains an `i2c0` shortcut.
OneWire bus tables include `pin`. Named OneWire operations take and release an
exclusive lease automatically; `solaros.onewire` remains the direct-pin
compatibility API. UART bus tables include `port`, `tx_pin`, `rx_pin`, and
`baud_rate`; named UART I/O takes and releases an exclusive lease automatically.

`create_i2c` requires `port`, `sda`, and `scl`; optional `speed_hz` defaults to
100000. `create_onewire` requires `pin`. Both claim their approved runtime pins
until `remove(name)`.

`create_uart` requires `port`, `tx`, and `rx`; optional `baud_rate` defaults to
115200. Its driver starts on first lease and stops after the final runtime-bus
lease is released.

```lua
local solaros = require("solaros")

local bus = solaros.buses.create_spi("spi1", {
    host = solaros.buses.SPI3_HOST,
    sclk = 1,
    mosi = 2,
    miso = 3,
    cs = {17},
})
print(bus.name, bus.origin)

local reply = solaros.buses.spi_xfer("spi1", "gpio17", "\x9f\x00\x00\x00")
print(#reply)
solaros.buses.remove("spi1")
```

```lua
local solaros = require("solaros")

local i2c1 = solaros.buses.create_i2c("i2c1", {
    port = 1,
    sda = 14,
    scl = 15,
    speed_hz = 100000,
})
print(#solaros.buses.i2c_scan(i2c1.name))
solaros.buses.remove(i2c1.name)

local onewire0 = solaros.buses.create_onewire("onewire0", {pin = 16})
print(#solaros.buses.onewire_scan(onewire0.name))
solaros.buses.remove(onewire0.name)

local uart1 = solaros.buses.create_uart("uart1", {
    port = 1,
    tx = 14,
    rx = 15,
    baud_rate = 115200,
})
solaros.buses.uart_write(uart1.name, "AT\r\n")
print(solaros.buses.uart_read(uart1.name, 64, 500))
solaros.buses.remove(uart1.name)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("i2c0")
print(bus.name, bus.speed_hz)
local addresses = solaros.buses.i2c_scan("i2c0")
solaros.buses.i2c_probe("i2c0", 0x3c)
```

```lua
local solaros = require("solaros")

local bus = solaros.buses.get("onewire0")
print(bus.name, bus.pin)
local devices = solaros.buses.onewire_scan("onewire0")
local reply = solaros.buses.onewire_xfer("onewire0", 9, "\xcc\x44")
```

`solaros.expansion.drivers()` lists compiled drivers, and `devices()` lists
active devices with normalized bindings. `attach(driver, name, bindings)` and
`detach(name)` mirror the shell lifecycle. Binding tables accept `spi`, `cs`
(or `ce`), `i2c`, `addr`, `uart`, `gpio`, `irq`, `reset` (or `rst`), `dc`,
`busy`, `adc`, and `pwm`. `cs` requires `spi`, `addr` requires `i2c`, and
unknown fields are rejected.

```lua
solaros.expansion.attach("pcd8544", "lcd0", {
    spi = "spi0",
    cs = 10,
    dc = 4,
    reset = 5,
})
print(#solaros.expansion.devices())
solaros.expansion.detach("lcd0")
```

`solaros.spi` is a compatibility table that selects `spi0` when present,
otherwise the first registered named SPI bus. On a dynamic-only board its
`status().available` value remains false until a bus is created. `status()`
reports the selected bus pins, transfer limit, and configured chip-select
slots. `xfer(cs, data[, mode[, speed_hz]])` performs a full-duplex transaction.
`read(cs, length[, fill[, mode[, speed_hz]]])` and
`write(cs, data[, mode[, speed_hz]])` provide one-direction convenience forms.
The `cs` argument accepts a configured slot name or its numeric GPIO. Lua data
and return values are binary-safe strings. New code should address buses
explicitly through `solaros.buses.spi_*`.

`solaros.uart` is the default `uart0` compatibility table; use
`solaros.buses.uart_*` for another named UART. `solaros.uart.status()` includes
the bus `name`, `rx_buffered`, and `rx_buffered_valid`. When another owner is
actively using the UART, `rx_buffered_valid` is `false` because the live RX
count is not sampled.

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

## Sessions

`solaros.sessions` creates manual port shell sessions and closes sessions by id.
Script-created port shells do not run `/.shell/startup`.

- `create_shell(port[, term[, cols, rows]])`: create a port shell and return its numeric session id.
- `create_shell(port, {term="auto", cols=80, rows=24})`: table-options form for the same call.
- `close(session_id)`: close a display/app session or stop a port shell session.

Example:

```lua
local solaros = require("solaros")

pcall(function()
    solaros.jobs.stop("slip")
end)

local sid = solaros.sessions.create_shell("uart0", {term = "auto"})
-- later:
solaros.sessions.close(sid)
solaros.jobs.start("slip", {"uart0", "115200"})
```

## Graphics

`solaros.gfx` draws through the foreground graphics service. `begin(target)` claims a named display target, such as `lcd0`, until `end()` or script cleanup. Colors are `WHITE`, `LIGHT`, `DARK`, `BLACK`, and `gray(level)` with `0..GRAY_MAX`. Fonts are `FONT_SMALL`, `FONT_MONO`, `FONT_BOLD`, regular document fonts `FONT_MONO_12` through `FONT_MONO_20`, bold document fonts `FONT_BOLD_12` through `FONT_BOLD_20`, and matching italic/bold-italic constants. Italic constants currently map to the closest upright face in the trimmed firmware font set.

Functions:

- `begin([target])`, `end()`
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

For an attached auxiliary display, use the target name:

```lua
gfx.begin("lcd0")
gfx.clear(gfx.WHITE)
gfx.text(2, 14, "aux")
gfx.present()
gfx["end"]()
```

## Notes

Lua tables returned as lists use normal Lua 1-based array indexes. Direct block lookup with `solaros.storage.block(index)` follows the underlying storage service index, matching Python's 0-based `block(index)`.

The Lua bridge intentionally does not expose raw SSH/SCP session handles. Those need explicit object lifetime and event-loop rules before becoming scriptable.
