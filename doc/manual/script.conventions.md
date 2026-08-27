+++
id = "script.conventions"
title = "SolarOS scripting conventions"
section = "concept"
summary = "Write cooperative Python and Lua programs against SolarOS services"
aliases = ["scripting", "scripts"]
keywords = "python lua script conventions import require errors exit arguments runtime package"
packages_any = ["app_python", "app_lua"]
+++
# SolarOS scripting conventions

Python and Lua are the normal way to build custom SolarOS applications. Scripts
call native services through the `solaros` module rather than assuming Unix
process, filesystem, or device APIs.

## Start with discovery

Inspect the installed board, packages, buses, displays, and safe pins before
choosing names or hardware. Optional modules disappear when their package is
not compiled.

## Cooperate with the foreground session

Interactive code must check `solaros.should_exit()` and release displays,
buses, GPIO claims, files, and interpreter-owned services on every exit path.
Use `try/finally` in Python and `pcall` plus explicit cleanup in Lua.

## Read foreground input

Use `solaros.input.sources()` to discover pointer and axis sources, then use
`solaros.input.read(timeout_ms)` for touch coordinates, mouse deltas and
buttons, or joystick axes. The input queue belongs to the foreground Python or
Lua application. Headless source runners do not receive these events. Keep the
timeout bounded so the loop can check `solaros.should_exit()` regularly.

Keyboard characters and navigation keys use `solaros.tui.getch()` instead of
the pointer and axis queue. See the Python or Lua API reference for event fields,
constants, queue capacity, and overflow reporting.

## Run a saved script

```text
python /app.py argument
lua /app.lua argument
```

Python arguments are in `sys.argv`. Lua arguments follow the runtime's standard
argument table.

## File argument convention

Scripts that accept one primary input file should use the canonical option
`--file PATH`. The option is not mandatory for scripts that do not consume a
file. Playground recognizes this exact option and completes filesystem paths
for its following argument:

```text
playground run APP-ID --file PATH
```

Document `--file` in the application's README when it is supported. The
manifest does not need a separate argument declaration.

## Quick reference

Python imports the native solaros module; arguments are in sys.argv. Lua uses
the preloaded global solaros or require with the module name. Mutating service
failures surface as SolarOS errors. Optional modules are package-gated.
Interactive code should check solaros.should_exit(). Use SolarOS service APIs
instead of assuming Unix process, filesystem, or device APIs. Use `--file PATH`
for a primary input file. Foreground pointer and axis events use solaros.input;
keyboard characters use solaros.tui.getch().
