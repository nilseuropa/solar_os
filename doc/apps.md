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
known source files.

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

## irriga

Graphical front-end for the irrigation controller. It shows the clock and all
zone states live, and edits schedules, zone count, mode, and the RTC. The
schedule engine itself runs as the `irrigd` background job; the app is only a
viewer/editor, so exiting it never changes relay outputs.

Usage:

```text
irriga
```

Controls:

- Home: `Up`/`Down`/`Left`/`Right` select a zone (the dashed focus frame
  appears on the first key press and hides 5 seconds after the last one),
  `Enter` opens the zone editor, `Space` toggles the zone's manual switch,
  `m` toggles auto/manual mode, `s` opens settings, `Esc` exits.
- Zone editor (Casio style): the zone's four slots on the left with the
  edited field inverted; `Left`/`Right` (or NEXT) walk the fields --
  start/end time, weekday toggles, active flag -- across all slots,
  `Up`/`Down` (or +1/-1) adjust, `Enter` (or SET) commits the zone, `Esc`
  (or BACK) returns without committing pending edits.
- Settings: zone count and mode with `Left`/`Right`, `Enter` on "Set clock"
  opens a manual RTC editor for offline sites (weekday is derived
  automatically).

On boards with the FT6336 touch panel (M5Stack Core2/CoreS3) the app is also
tap-driven: tapping a zone's lettered button or panel on the home screen
opens its editor, and the editor's BACK/SET/NEXT/+1/-1 buttons and slot rows
respond to taps. Touch assumes the default landscape orientation.

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

## webradio

Internet radio player for MP3 streams, on the same audio pipeline as `aplay`.
A network task fills a PSRAM jitter buffer while the decode task plays from
it, so short network hiccups do not interrupt playback.

Usage:

```text
webradio <mp3-stream-url> [-v volume]
```

Example:

```text
webradio http://icecast.example.com/station.mp3 -v 80
```

Controls / notes:

- `Esc` stops playback; the app-exit key exits.
- MP3 streams only (HTTP or HTTPS); no AAC or HLS playlists.
- Wi-Fi power save is reduced during playback and restored on stop.
