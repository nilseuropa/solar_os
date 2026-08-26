+++
id = "sessions.apps"
title = "Foreground sessions and applications"
section = "service"
summary = "Create shells and inspect resumable foreground applications"
aliases = ["sessions"]
keywords = "python lua sessions apps shell port terminal create close focus input ble registry foreground"
packages_any = []
+++
# Foreground sessions and applications

A session is a foreground application or shell attached to a display or byte
stream. Each display or port has one active view, but can retain several
suspended applications. Sessions let you switch away and return without
discarding their state.

## Inspect and switch

```text
session list
session fg 3
session send 3 wifi status
session close 3
```

`session send ID COMMAND` runs a command on an active display shell from any
other shell. The command is echoed and added to the target shell's history, and
its output stays on the target display. SolarOS refuses the request if the
target is an application, is suspended, has a partially typed command, or is
the calling shell.

`Alt+Tab` or `Alt+Right` switches to the next session on the locally focused
display. `Alt+Left` switches to the previous session. Either Alt key is
accepted, including AltGr on compact keyboards. Both directions follow wrapped
session-ID order, and the display shell remains in the cycle. `session fg`
explicitly foregrounds its session and moves local input focus to that
session's display. Closing an application returns to that display's shell.

On UART, USB CDC, Telnet, and other port shells, press `Ctrl+Z` to suspend a
resumable foreground application and return to the prompt. Run `fg` to restore
the most recently suspended application on that port, or `fg ID` to restore a
specific port-owned session. `Ctrl+]` still closes the current application.
Port sessions never migrate: a command from another shell can foreground or
close one, but it changes the application shown on the owning port.

Applications currently remain single-instance across the device. If an app is
already retained on another display or port, SolarOS reports its session owner
instead of creating a second copy.

SolarOS always keeps at least one interactive shell. `exit`, `close`, and
`session close` refuse to close the final remaining display or port shell.

Native applications receive cooperative tick events every 25 ms by default.
An application that needs a faster best-effort update rate can set its
descriptor's `tick_interval_ms` below 25; the runtime raises its scheduler
cadence while that application is active. A zero value keeps the default.

## Create another shell

```text
session create shell cdc0 --term auto
session create shell uart0 --term ansi --charset ascii --size 80x25
session create shell lcd0
```

Use `port list` or `display list` to discover real targets first. A manually
created port session does not rerun `/.shell/startup`. A display shell is
created on its target without changing local input focus; use `session focus`
when the BLE keyboard or board controls should move to it.

Terminal control and character encoding are separate. `--term` selects cursor
and screen-control behavior. Port shells use `--charset utf8` by default; add
`--charset ascii` for DOS or another legacy terminal so TUI line art and
symbols are rendered with ASCII fallbacks. The setting does not change
framebuffer display sessions.

Cursor-addressable port shells echo ordinary end-of-line typing directly and
send required line edits as one redraw. This keeps the cursor stable on UART,
USB CDC, and network-backed terminals such as Telnet.

## Start an app on a display

An app can be launched onto a named display from any shell:

```text
session create files display0
session create reader oled0 /manual.md
session create writer display0 /notes.md
```

The invoking port shell immediately returns to its own prompt. The new app
receives input belonging to its display; serial input remains with the serial
shell. App arguments use the invoking shell's current directory for the same
path-aware apps as a direct launch.

This cross-shell display control remains available while a port shell retains
its own applications. `session close ID` can close either kind of session.

## Child applications

Foreground applications can temporarily open another app on the same terminal.
Files uses this for Reader, Less, Edit, and View; Playground uses it to run an
installed Python or Lua application. The parent is suspended while the child is
active. Closing the child resumes the parent. Suspending the child with
`Ctrl+Z` returns to the shell and retains both sessions for later `fg`.

## Local input focus

Keyboard, board-button, and D-pad key events share one local display focus.
Pointer events without an explicit target use that focus too. Analog joystick
axes remain semantic axis input and are not converted to focus keys. The
primary board display is selected at boot. On a headless board, the first
display shell created becomes the default.

```text
session focus
session focus oled0
```

Changing focus does not move or restart a session. It only assigns local input
and target-scoped session-cycle shortcuts. Browser input from `displayd`
remains assigned to the display being controlled.

## Quick reference

solaros.sessions.create_shell(port, optional term, cols, rows, charset) returns
a session id; close(id) closes it. The Lua options table and Python keyword
form also accept `charset="utf8"` or `charset="ascii"`. Script-created port
shells do not run /.shell/startup. solaros.apps.list() and find(name) inspect
registered foreground apps.
