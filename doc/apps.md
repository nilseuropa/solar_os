# SolarOS Embedded Apps

This document covers foreground applications registered in SolarOS. Availability
depends on the selected firmware flavor and board capabilities. The `apps`
command shows the app set compiled into the running image.

Exit behavior:

- Display shell: `CTRL+ALT+DEL` exits foreground apps.
- Port shells: `Ctrl+]` exits foreground apps.
- `Alt+Tab` switches between resumable foreground sessions on display builds.

## aplay

Play audio files through the board audio output. WAV and MP3 are supported when
the audio package is compiled and the board has audio hardware.

Usage:

```text
aplay [-v volume] file.wav|file.mp3
```

Controls:

- App-exit key stops playback.

## arecord

Record microphone input to a WAV file. This requires the audio package and
board microphone hardware.

Usage:

```text
arecord [-d seconds] file.wav
```

Controls:

- App-exit key stops recording.

## chat

Two-pane gateway chat client. The left pane lists channels, the right pane shows
conversation history, and the bottom line is the message/command input.

Usage:

```text
chat [gateway-url] [channel] [user] [token]
```

In-app commands:

```text
/help
/join channel
/leave [channel]
/delete [channel]
/connect [url]
/disconnect
/status
/quit
```

Controls:

- `Tab` changes focus between channel list, messages, and input.
- `Up`/`Down` navigate the focused pane or input history.
- `Enter` sends input or joins the selected channel.
- `Page Up`/`Page Down` scroll messages.
- `Esc` or app-exit key exits.

## clock

Full-screen graphical seven-segment clock, alarm countdown, and stopwatch.

Usage:

```text
clock
clock -a mm:ss
clock -s
```

Controls:

- In stopwatch mode, `Space` starts/stops.
- In stopwatch mode, any other ordinary key resets to zero.
- `Esc` or app-exit key exits.

## com

Serial terminal for the expansion UART. BLE keyboard input is forwarded to the
UART and UART RX is drawn in the terminal.

Usage:

```text
com
```

Controls:

- App-exit key exits.

## curl

HTTP client for quick text downloads and diagnostics. It can print response
data to the terminal or save it to a file.

Usage:

```text
curl [-L] [-o file] URL
```

Controls:

- App-exit key cancels an active transfer.

## edit

Text editor for files on mounted storage. It supports cursor navigation,
selection, clipboard operations, text-size changes, and syntax highlighting for
known source files. The editor supports files up to 256 KiB on boards with
PSRAM and 32 KiB on boards without PSRAM.

Usage:

```text
edit <file>
```

Controls:

- Arrows move the cursor.
- `Ctrl+Left`/`Ctrl+Right` move by words.
- `Shift+Arrows` extend selection.
- `Ctrl+A`, `Ctrl+C`, `Ctrl+X`, `Ctrl+V` select all, copy, cut, and paste.
- `Ctrl++` and `Ctrl+-` adjust editor text size for the active session.
- `Esc` saves if needed and exits; app-exit key exits.

## files

Two-pane file manager inspired by Midnight Commander. It is intended for quick
copy, move, delete, and launch workflows on mounted storage.

Usage:

```text
files [path]
```

Controls:

- Arrows navigate panes.
- `Tab` switches active pane.
- `Enter` opens directories or launches known files.
- File operations refresh both panes after completion.
- App-exit key exits.

## inbox

Universal incoming-message browser for pages, chat notifications, mail, and
other background producers. It reads the same shared inbox that supplies the
status-bar unread count.

Usage:

```text
inbox
```

Controls:

- `Up`/`Down`, `Page Up`/`Page Down`, `Home`, and `End` navigate.
- `Enter` or `Right` opens the selected message and marks it read.
- `Left`, `Backspace`, or `Esc` returns from a message to the list.
- `u` toggles the unread-only filter.
- `m` toggles the selected message between read and unread.
- `r` refreshes and `q` or the app-exit key exits.

## email

Receive-only IMAPS client for the configured mailbox. The app shows the
provider-specific message list while every newly synchronized message is also
published to the universal inbox and its shared status-bar unread counter.

Configure and synchronize the account before opening the app:

```text
wifi on
email configure imaps://imap.example.com user@example.com app-password INBOX
email sync
email
```

Controls:

- `Up`/`Down`, `Page Up`/`Page Down`, `Home`, and `End` navigate.
- `Enter` or `Right` opens the selected message and marks its universal inbox
  notification read.
- `Left`, `Backspace`, or `Esc` returns from a message to the list.
- `u` toggles the unread-only filter.
- `m` toggles the selected message between read and unread.
- `r` refreshes and `q` or the app-exit key exits.

The account configuration persists in NVS. The local message list is volatile
and keeps the newest 32 synchronized messages. This first version displays a
best-effort text preview; MIME attachments, encoded headers, sending, and
server-side read flags are not implemented yet.

## io

Interactive expansion I/O manager. It presents the board's expansion pins,
named buses, and resource claims in one TUI and uses the same ownership and
validation services as the `gpio`, `i2c`, `spi`, `uart`, `onewire`, and
`expansion` commands.

Usage:

```text
io
```

Controls:

- `Tab`, `Left`, and `Right` switch between Pins, Buses, and Claims.
- Arrows, Page Up/Page Down, Home, and End navigate the selected view.
- `Enter` opens context-sensitive actions for a pin or bus.
- `n` creates a board-approved named I2C, SPI, UART, or 1-Wire bus.
- Bus creation uses arrows to select fields and values; the generated bus name
  can be edited directly.
- Runtime buses can be attached, detached, or removed when their lease state
  permits it. Their `Autostart` action idempotently appends the matching
  `expansion bus create ...` command to `/.shell/startup`. Direct GPIO and PWM
  assignments can be created and released from the Pins view.
- `r` refreshes; `q`, `Esc`, or the app-exit key exits.

## invaders

Graphical arcade shooter.

Usage:

```text
invaders
```

Controls:

- `Left`/`Right` move the ship.
- `Space` or `f` fires.
- `Esc` or app-exit key exits.

## less

Terminal pager for text files. It preserves original text layout and is useful
for quick file inspection.

Usage:

```text
less <file>
```

Controls:

- `Up`/`Down` or `j`/`k` scroll one line.
- `Page Up`/`Page Down`, `b`, or `Space` page.
- `Home`/`End` or `g`/`G` jump to start/end.
- `/` starts search, `n`/`N` repeat search.
- `q`, `Esc`, or app-exit key exits.

## logic

On-device logic analyzer waveform viewer. It displays the latest capture made
by the shared logic analyzer service or the SUMP job. With pin arguments it
makes a new local capture before opening the viewer.

Usage:

```text
logic
logic <pin[,pin...]> [rate-hz] [samples]
```

Examples:

```text
logic
logic 1,2,3,17
logic 1,2 500000 8192
```

Controls:

- Left/Right pans through the capture.
- `+`/`-` or Page Up/Page Down changes the time scale.
- `r` captures again with the current local configuration when the SUMP job is
  stopped.
- `a` or Home shows the complete capture.
- `q`, Esc, or the app-exit key exits.

The app is compiled only for boards with graphics and runtime-safe GPIOs. While
the SUMP job is running, the app remains a viewer and automatically reloads
captures received from the host.

## lua

Embedded Lua runtime. It can run an interactive REPL or execute `.lua` scripts
from storage. Lua scripts can use SolarOS service bindings when the selected
firmware includes the corresponding packages.

Usage:

```text
lua
lua file.lua [args...]
```

Controls:

- `exit()` returns from the REPL.
- App-exit key interrupts running code or exits.

## notes

Markdown-backed checklist and category manager. It stores unchecked and checked
items and supports one level of category folding.

Usage:

```text
notes [file.md]
```

Controls:

- `Up`/`Down` navigate.
- `Space` toggles an item.
- `a` adds an item.
- `c` adds a category.
- `Enter` edits the selected line.
- `d` or `Delete` deletes the selected item/category.
- `Shift+Up`/`Shift+Down` reorders items within a category.
- `Left`/`Right` collapse/expand a category.
- `q`, `Esc`, or app-exit key exits.

## plot

Graphical plotter for DAQ CSV files and live scalar streams. It is compatible
with CSV generated by the `daq` job.

Usage:

```text
plot <scalar-stream...> [--rate ms]
plot -f <file.csv> [column...]
```

Examples:

```text
plot temperature humidity --rate 1000
plot -f /logs/env.csv temperature humidity
```

Controls:

- `Left`/`Right` pan.
- `Up`/`Down` select series.
- `+`/`-` or `Page Up`/`Page Down` adjust visible window/zoom.
- `a` or `r` resets the view.
- `Space` pauses/resumes live mode.
- `q`, `Esc`, or app-exit key exits.

## python

Embedded MicroPython runtime. It can run an interactive REPL, `.py` scripts, or
`.mpy` files from storage. Python scripts can use SolarOS service bindings when
the selected firmware includes the corresponding packages.

Usage:

```text
python
python file.py [args...]
python file.mpy [args...]
```

Controls:

- `exit()` returns from the REPL.
- App-exit key interrupts running code or exits.

## reader

Graphical document reader for plain text, Markdown, and EPUB. It remembers
reading position and zoom per opened file when storage is available.

Usage:

```text
reader <file.txt|file.md|file.epub>
```

Controls:

- `Up`/`Down` scroll by line.
- `Page Up`/`Page Down` page.
- `Home`/`End` jump to start/end.
- `+`/`-` adjust zoom.
- `/` starts search, `n`/`N` repeat search.
- `Esc` exits search state first; otherwise exits.
- `q` or app-exit key exits.

## scp

SCP file transfer over SSH. It supports password or key authentication through
the shared SSH transport and host lookup/known-host storage.

Usage:

```text
scp [-P port] local [user@]host:remote
scp [-P port] local [user@]host:
scp [-P port] [user@]host:remote local
scp [-P port] [user@]host:remote-glob dir
scp [-P port] [user@]host:remote
```

Remote download paths can use `*` or `?`. The local target must be an existing
directory for remote wildcard downloads.

Controls:

- App-exit key cancels an active transfer.

## sheet

CSV viewer for small data tables. It is intended as a companion to `daq` logs
and simple spreadsheet-like inspection.

Usage:

```text
sheet <file.csv>
```

Controls:

- Arrows move through cells.
- Page keys scroll.
- App-exit key exits.

## ssh

Interactive SSH client. It supports password and key authentication, known
hosts, hostname lookup through `/.ssh/hosts`, UTF-8 text, VT-style controls, and
remote full-screen terminal applications.

Usage:

```text
ssh [user@]host [port]
```

Controls:

- Most keys are sent to the remote host, including Esc, Alt+key, cursor keys,
  Ctrl combinations, and function keys.
- App-exit key closes the SSH app.
- `Alt+Tab` leaves the session running in the background on display builds.

## telnet

Telnet client for classic TCP terminal sessions. It supports basic Telnet
option negotiation, terminal type reporting, window size reporting, and raw mode.

Usage:

```text
telnet host [port]
telnet -r host [port]
```

Controls:

- App-exit key closes the Telnet app.
- `Alt+Tab` leaves the session running in the background on display builds.

## view

Graphical image viewer. It supports the image formats compiled into the current
firmware, including common PNG/JPEG/GIF/WebP paths and automatic animated GIF
playback when the media package is enabled.

Usage:

```text
view [-fit|-actual] <image>
```

Controls:

- Arrows pan.
- `f` toggles fit/actual mode. Fit mode scales the image up or down to the screen.
- `0` selects actual size.
- `1` selects fit-to-screen.
- `Esc` or app-exit key exits.

## web

Simple graphical web browser for lightweight HTML pages. It shares document and
image rendering infrastructure with `reader` where possible.

Usage:

```text
web http://host/
web https://host/path
```

Controls:

- Keyboard navigation follows the active web UI state.
- `Esc` or app-exit key exits.
