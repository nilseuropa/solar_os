# Lua runtime

Install with `pkg install lua` on SolarOS ESP32-S3 firmware with PSRAM and
native modules. Run `lua` for the REPL or `lua SCRIPT.lua [ARG...]` for a
script. Playground uses the installed interpreter for Lua applications.
Install Jet separately to run Boing.

The package contains the Lua VM, console, and Playground bindings for graphics,
keyboard input, timing, tones, synthesis, and Jet scenes. The firmware-selected
Lua runtime provides the full scripting API.

Build the package with the ESP32-S3 compiler on PATH:

```sh
cmake -S modules/lua -B /tmp/solaros-lua
cmake --build /tmp/solaros-lua --target elf
```

The build validates `lua.app.elf` with SolarOS's ELF validator. Lua sources
are shared with the built-in interpreter. The application uses only SolarOS
public service tables and does not use a board's firmware build products.
