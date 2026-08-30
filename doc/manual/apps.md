+++
id = "apps"
title = "Application reference"
section = "app"
summary = "Usage, controls, and examples for every foreground application"
aliases = ["applications"]
keywords = "apps applications foreground controls usage examples reader writer sketch paint pointer png markdown less files edit hexedit binary agent calculator calc graph webradio radio mp3 function generator funcgen waveform sweep"
packages_any = []
+++
# SolarOS Embedded Apps

This document covers foreground applications registered in SolarOS. Availability
depends on the selected firmware flavor and board capabilities. The `apps`
command shows the app set compiled into the running image.

Exit behavior:

- Display shell: `CTRL+ALT+DEL` exits foreground apps.
- Port shells: `Ctrl+]` exits foreground apps.
- Port shells: `Ctrl+Z` suspends a resumable app and returns to the prompt;
  `fg` restores the most recently suspended app.
- `Alt+Tab` or `Alt+Right` switches to the next resumable foreground session on
  the locally focused display. `Alt+Left` switches to the previous session.
  Either Alt key is accepted, including AltGr on compact keyboards.

## agent

Native Responses/Chat-Completions LLM client and SolarOS agent control plane.
It streams model text directly to the active shell and exposes typed
system-status, storage-listing, job-listing, display-discovery, and optional
Python/Lua execution tools. `agent tools` shows risk, policy, and runtime
availability.

Configure the full endpoint and model, then ask a question:

```text
agent config endpoint https://api.openai.com/v1/responses
agent config model gpt-model
agent config key api-key
agent config reasoning medium
agent config tools confirm
agent config max-tools 16
agent
agent list
agent resume SLOT
agent ask How much memory is free on this device?
agent script python -c "print(6 * 7)"
agent script lua /script.lua argument
```

Bare `agent` and `agent new` open a new foreground prompt loop. Completed turns
are saved, and `agent list` plus `agent resume SLOT` restore a selected transcript
after leaving the app or rebooting. `agent delete SLOT` removes one conversation
without changing provider credentials. Responses uses its saved provider
continuation ID; Chat Completions uses bounded local history. Slots are `1` to
`3` on internal flash or `1` to `8` on SD; the oldest is reused when full.
`agent ask`
performs one unsaved request but likewise waits for `Esc` or the app-exit key
after completion, so display-shell output is not immediately replaced by the
shell screen.

Use `agent config key clear` for an endpoint that does not require bearer
authentication. `agent status` shows configuration, request counters, HTTP
status, reasoning effort, duration, traffic, and internal-RAM/PSRAM measurements
from the last request. The API key itself is never printed. A
Chat-Completions-compatible endpoint can still be configured explicitly.

The agent permits 16 sequential tool calls by default, followed by a separately
reserved final provider turn. `agent config max-tools COUNT` stores a
per-request limit from 1 through 32 in NVS. The final turn advertises no tools
after that budget is consumed, so the model concludes from the collected
results instead of failing on one more tool request. Output is bounded to
16 KiB, protocol buffers and queues prefer PSRAM, and the foreground worker
uses a declared 16 KiB internal stack.
Full builds can reuse that worker for bounded Python or Lua source/file
execution. The manual script path captures at most 4095 output bytes, has a
30-second deadline, and supports cancellation with `Esc` or the app-exit key.
Model-generated source is capped at 640 bytes and captures 383 output bytes.

The storage registry includes bounded listing, sensitive text-file reads, and
text-file replacement. Reads and writes are capped at 3072 bytes and paths
below `.ssh` are unavailable to protect SSH identity files.

The default `confirm` policy runs read-only tools automatically and shows the
exact arguments of sensitive, mutating, disruptive, and script calls before
waiting for a local `y/N` decision. `off` disables tools, `readonly` excludes
all protected risk classes, and explicit `all` allows every available tool
without prompting.
See [Native Agent Service](agent.md) for the provider contract and current
limits.

Controls:

- `Esc` or the app-exit key cancels an active request and exits.
- `Page Up`/`Page Down` scroll terminal output while a request is active.

## aplay

Play audio files through the default registered playback endpoint. WAV and MP3
are supported when an output device is present. MP3 decoding is provided by the
shared, device-independent audio codec service. `aplay` prints the source
details, plays the file once through the shared background audio player, and
then returns to the prompt without clearing existing terminal output. It can be
launched from display, UART, USB CDC, Telnet, and other port shells.

Usage:

```text
aplay [-v volume] file.wav|file.mp3
```

Controls:

- `Esc` or app-exit stops playback early and returns to the prompt.

## arecord

Record the default registered capture endpoint to a WAV file. This requires a
registered input device; it does not require built-in board audio. With `-d`,
recording stops after the specified number of seconds. Without `-d`, recording
continues until app-exit, the storage fills, or the WAV size limit is reached.
It can be launched from display, UART, USB CDC, Telnet, and other port shells.

Usage:

```text
arecord [-d seconds] file.wav
```

Controls:

- App-exit stops recording, finalizes the WAV header, and returns to the prompt.

## recorder

Interactive GUI/TUI counterpart to `arecord`. Recorder writes PCM WAV files so
the channel count, sample rate, and resolution travel with the recording and
the result can be played immediately. It accepts any registered signed-16-bit
PCM capture stream. On the Waveshare board it initially selects
`audio0.capture`. Mono/stereo output, 8/16-bit file resolution, and sample rates
from 8 kHz through 48 kHz are converted from the selected stream as necessary.

Usage:

```text
recorder [--tui] [file.wav]
```

The optional path supplies the initial recording folder and filename. The
Setup view can edit the filename or select another destination directory with
the shared file browser. When the filename is empty, each recording gets a
unique local-time name such as `rec-260810-113045.wav`; a numeric suffix avoids
overwriting a file created in the same second. Recorder remembers its selected
input, folder, format, hardware input gain, output volume, and visualizer in
`.recorder/settings.bin` on the current storage root. An explicit path argument
overrides the remembered folder. The filename is not remembered, so reopening
Recorder cannot accidentally reuse a one-off name.

Recorder selects its graphical interface on a graphical shell and its text
interface on a port shell. `--tui` forces the text interface even when the
launching shell has graphics.

`Tab` switches between Record and Setup. The Record view uses the shared
cassette widget by default; `V` cycles through Cassette, Oscilloscope, and
Spectrum. `R` starts recording, Space pauses or resumes, `S` stops and
finalizes the WAV header, and `P` plays the most recent or selected recording.
`M` starts or stops monitoring: it continuously streams the selected input to
the default output and visualizer. Monitoring can remain enabled when recording
starts and can be toggled while the WAV writer continues. `Up`/`Down` changes
monitor and playback volume. The cassette reels move during recording and
playback, but remain still during monitoring. Oscilloscope and Spectrum remain
live during monitoring. Active visualizers are presented on the 40 ms app tick
(nominally 25 frames per second), independently of the audio block rate.

Setup selects the capture stream, recording folder, mono/stereo conversion,
sample rate, 8/16-bit WAV resolution, input gain, and output volume. Enter on
Folder opens the WAV-filtered browser. `D` selects its current directory as the
recording folder; Enter on a WAV file plays it and returns to Record. Input gain
is not a generic stream multiplier:
Recorder enables it only when the selected capture stream belongs to an audio
device that advertises hardware input-gain control. Thus `audio0.capture` on
the Waveshare controls the ES7210 microphone gain, while an unrelated generic
stream shows `Input gain: n/a`. The hardware gain is system-wide; Recorder does
not add hidden digital amplification to the saved PCM.

The text interface exposes the same setup values, browser, transport keys,
monitoring, pause behavior, and playback path. Recorder is resumable: capture,
monitoring, or playback continues while its UI session is in the background,
and closing the app stops the worker and finalizes an active recording.

## funcgen

Audio-only function generator built on the shared real-time Synth service.
It emits signed 16-bit stereo PCM and can use the default playback device or
an explicitly selected runtime playback stream, including an attached LEDC PWM
audio expansion.

Usage:

```text
funcgen [--tui]
```

The graphical interface shows the exact generated PCM in the shared
oscilloscope widget. The remaining controls select sine, square, triangle, saw,
pulse, or noise output; frequency from 20 through 8000 Hz; amplitude; pulse
width; a repeating linear sweep; sweep end frequency; sweep time; and playback
stream. The TUI exposes the same controls. `--tui` forces it on a graphical
shell.

Controls:

- `Left`/`Right` selects a control.
- `Up`/`Down`, `+`/`-`, or `Enter` adjusts the selected control.
- `Space` starts or stops output.
- `Esc`, `Q`, or the app-exit key closes the application.

The output is off initially. Frequency is capped below the selected stream's
Nyquist limit during rendering. A sweep moves linearly from Frequency to Sweep
end during Sweep time, then repeats without resetting oscillator phase.
Suspending the UI leaves an active generator running; closing it stops the
Synth worker and releases the stream.

All controls can be targeted by the shared control-binding system while
Funcgen is active:

- `funcgen.waveform`: 0 sine, 1 square, 2 triangle, 3 saw, 4 pulse, 5 noise.
- `funcgen.frequency`: 20 through 8000 Hz, logarithmic.
- `funcgen.amplitude`: 0 through 100 percent.
- `funcgen.pulse.width`: 1 through 99 percent.
- `funcgen.sweep.enabled`: 0 off or 1 on.
- `funcgen.sweep.end`: 20 through 8000 Hz, logarithmic.
- `funcgen.sweep.time`: 100 through 60000 ms, logarithmic.
- `funcgen.output`: runtime output index; 0 follows the default output.
- `funcgen.enabled`: 0 off or 1 on.

The durable foreground state and oscilloscope storage use PSRAM when present.
Only the bounded oscillator/render state, Synth worker stack, and PCM block
remain in internal SRAM.

## player

Interactive WAV/MP3 player and the user-facing counterpart to `aplay`. `player`
keeps a persistent playlist under `.player` on the current storage root. Opening
an audio file from Files adds it to that playlist, selects it, and starts
playback. Missing files remain listed so removable media can be reattached.

Usage:

```text
player [--tui] [file.wav|file.mp3]
```

Player selects its graphical interface on a graphical shell and its text
interface on a port shell. `--tui` forces the text interface even when the
launching shell has graphics.

On a graphical session, `Tab` switches between Play and Playlist. The Play tab
uses the top two-thirds for a cassette visualizer by default; `V` cycles through
Cassette, Oscilloscope, and Spectrum. The cassette reels turn only while audio
plays and show track progress when duration is known. `Left`/`Right` plays the
previous or next track in the playlist ring, `Enter` plays or stops, Space
pauses or resumes, and `Up`/`Down` adjusts volume. On the Playlist tab,
`Up`/`Down` selects, `Enter` starts the track and returns to Play, `A` opens the
WAV/MP3 file browser, and `Delete` removes the selected playlist entry.

The text interface is one playlist screen: `Up`/`Down` selects, `Enter` plays
or stops, Space pauses or resumes, `A` opens the filtered file browser,
`Delete` removes an entry, and `Esc` exits. Its bottom status line shows the
playing, paused, or stopped state with elapsed and total time. Playback follows
the resumable app while another foreground session is selected and stops when
Player closes. End of file advances to the next playlist entry.

WAV playback converts the file's mono/stereo channel count and sample rate to
the selected output stream. A mono recording therefore plays through a fixed
stereo device without changing the recording stored on disk.

## calc

Scientific calculator and function plotter. On a graphical display, `calc`
opens an expression list beside a Cartesian plot. From UART, USB CDC, Telnet,
or any other text-only shell, the same command opens a scientific REPL without
the plot pane. `calc --tui` forces that REPL even when graphics are available.

The expression engine supports `+`, `-`, `*`, `/`, `%`, powers with `^`,
parentheses, scientific notation, and implicit multiplication such as `2pi` or
`2(x + 1)`. Built-ins include `sin`, `cos`, `tan`, their inverse functions,
`sqrt`, `abs`, `exp`, `ln`, `log`, `floor`, `ceil`, `round`, `min`, `max`,
`pow`, and `atan2`. Trigonometric input is in radians; `rad(degrees)` and
`deg(radians)` convert explicitly. Constants are `pi` and `e`.

Rows can hold scalar calculations, variables, one-argument functions, or plots:

```text
a = 2
f(x) = sin(x) / x
f(pi / a)
y = f(x)
```

For a one-off shell calculation, use:

```text
calc -e "sqrt(2)^2"
```

Text REPL commands:

- `:list` or `:vars` shows worksheet rows and scalar results.
- `:del N` removes a row; `:clear` empties the worksheet.
- `:save [file]` and `:load [file]` write or restore one expression per line.
  The default file is `calc.txt` in the current shell directory.
- `:help` shows the compact reference; `:quit` returns to the shell.
- Up/Down recalls input history; Left/Right, Home, End, Backspace, and Delete
  edit the current line.

Graphical controls:

- The expression editor keeps a white background for legible small text and
  outlines the active row with a thin dark border.
- Type to edit the selected row; Up/Down changes rows and Enter adds a row.
- Left/Right, Home, End, Backspace, and Delete edit within a row.
- Page Up toggles the selected graph row.
- Tab moves between the expression list and graph.
- In the graph, arrows pan, `+`/`-` zoom, and Home or `0` resets the view.
  Press `t` for a numeric trace cursor; Left/Right moves it and Up/Down selects
  another plotted row. Press `t` again to resume panning.
- Ctrl+S saves and Ctrl+O loads the default `calc.txt` worksheet.
- App-exit closes the calculator. The worksheet is kept only when explicitly
  saved.

## chat

Tabbed provider-neutral conversation client. The Channels tab lists gateway and
radio conversations. Enter selects a conversation and opens its bounded shared
history on the Chat tab, which also contains the message/command input. The app
opens and remains useful offline; network or radio transport jobs connect
independently.

Usage:

```text
chat [gateway|meshcore|link|conversation-id]
```

With no selector, Chat opens a unified view and initially selects the newest
unread conversation. A provider name filters the list. A decimal conversation
ID opens exactly that conversation.

The background `gateway-sync` job owns the gateway transport connection, retries, and
joined-channel replay. Each join sends the greatest stable message ID retained
for that room as its cursor; the gateway replays only newer messages. Requested
membership and server-confirmed membership are separate, connection-scoped
states. The shared messaging service owns queued outbound
messages and `gateway-sync` consumes only gateway requests. Start it explicitly with
`job start gateway-sync`, just like `email-sync`. Closing or suspending `chat` does
not disconnect an already-running synchronizer. Incoming messages remain in the
shared bounded messaging store and publish bounded notifications to the
universal inbox; reopening the app replays retained conversations from every
provider. With SD storage, full messages are retained under
`/.messages/messages.bin`. On
internal flash, Chat
restores the compact message copy already retained in `/.inbox/messages.bin`,
so it consumes no second flash ring. Both backends deduplicate transport replays
by stable message identity and keep linked Inbox read state aligned.

Unlike `email-sync`, `gateway-sync` takes no interval argument: it waits for Wi-Fi
and reconnects with exponential backoff while remaining in the running state.

Gateway setup and room lifecycle use `gateway status`, `gateway configure`,
`gateway connect`, `gateway disconnect`, `gateway rooms`, `gateway join`,
`gateway leave`, and `gateway delete`. Gateway synchronization runs only under
the `gateway-sync` job name. Selecting a known gateway room in the Channels tab
requests a join automatically and opens it after the server confirms `joined`.

Conversation rows show provider, unread, and security state. Outbound rows show
queued/sending/sent/delivered/failed state. Use `/new CONTACT_ID` to open a
direct conversation with the contact's preferred endpoint. Sending to a
discovered endpoint asks for a second Enter confirmation; blocked endpoints
cannot be messaged.

The conversation header reports the selected provider's state. Gateway uses
`disconnected`, `connecting`, and `connected`; connectionless MeshCore and Link
providers use `stopped`, `starting`, `ready`, and `error`.

In-app commands:

```text
/help
/new contact-id
/status
/quit
```

Controls:

- `Tab` switches between the Channels and Chat tabs.
- In Channels, `Up`/`Down` select a conversation and `Enter` joins a known
  gateway room if necessary, then opens the Chat tab.
- In Chat, `Up`/`Down` navigate input history and `Enter` sends input.
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

Serial terminal for a bidirectional byte-stream port. Display-keyboard or
port-shell input is forwarded to the selected port, and received bytes are
drawn in the active terminal. The port may be a UART or a virtual port such as
a peer-bound SolarOS Link stream.

Usage:

```text
com [--autobaud] [--hex] [port]
```

The port defaults to `uart0`. For example, `com gps` connects to an existing
runtime UART bus named `gps`, while `com vser0` opens a Link stream previously
created with `link stream create`. The selected port remains claimed by the app
until the session exits. `com` works from both display and port shells; when
launched from a port shell, its terminal output is returned through that same
port. The input/output shell port and selected COM port must be different.

`--autobaud` is available only for UART buses. It samples the RX signal for
three seconds before opening the terminal. Send a repeating `0x55` or `0xaa`
pattern during that interval. A
reliable measurement is matched to a standard UART rate and applied to the
current bus connection without overwriting the saved baud setting. UART input
forwarding begins when sampling completes. If measurement fails, the configured
rate is kept.

`--hex` displays received bytes as eight-byte offset, hexadecimal, and ASCII
rows instead of interpreting them as terminal text. Both options can be used
together.

Examples:

```text
com --hex gps
com --autobaud --hex uart0
com vser0
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

## webradio

Stream a direct MP3 URL through the default registered audio output. On a
graphical display shell, WebRadio opens a two-tab media-player GUI. On UART,
USB CDC, Telnet, SSH, and other text shells, it opens a station-list TUI.

The catalog is stored as `.solar/webradio/catalog.bin` on the current storage
root and starts with the Nightride, Chillsynth, Datawave, Spacesynth, Darksynth,
Horrorsynth, EBSM, and Rekt streams. Catalog changes survive reboot. On first
use after an upgrade, WebRadio moves an older NVS catalog to this file and
removes the large NVS blob. `reset` restores the initial list.

Usage:

```text
webradio
webradio --tui
webradio https://stream.nightride.fm/nightride.mp3
webradio list
webradio add MyStation https://example.net/live.mp3
webradio remove MyStation
webradio reset
```

`--tui` forces the station-list TUI even on a graphical shell and can be
combined with a direct stream URL. Catalog-management commands also accept it
as a harmless interface override.

URLs are literal HTTP or HTTPS MP3 stream URLs. WebRadio does not translate
station names or website addresses and does not discover streams from HTML
pages. The initial implementation does not support playlists, HLS, or AAC.

Controls:

- `Tab` switches between the Player and Channels tabs in the GUI.
- The Player tab gives the top two-thirds of the screen to a live PCM
  oscilloscope or spectrum analyzer. `V` switches visualizers. The spectrum
  analyzer uses the shared DSP service, including PIE SIMD window and FFT paths
  on eligible ESP32-S3 boards.
- On the Player tab, `Left` and `Right` play the previous or next catalog
  channel. The catalog wraps as a ring. Space or `Enter` stops or resumes
  playback, and `Up`/`Down` changes global volume in five-percent steps.
- The bottom third of the Player tab shows the channel, playback state, volume
  bar, and previous, stop/play, and next controls.
- On the Channels tab, `Up`/`Down` selects a channel, `A` adds one, `E` edits
  one, and `Delete` removes one. `Enter` starts the selected channel and returns
  to the Player tab. Add and edit dialogs accept a name followed by a literal
  stream URL.
- The TUI is a single catalog screen. `Up`/`Down` or `J`/`K` selects a channel,
  `+`/`-` changes output volume, `A` adds a channel, `E` edits it, and `Delete`
  removes it. `Enter` plays the selected channel, Space stops playback, and
  `R` reconnects it. Add and edit use inline name and URL fields on the catalog
  screen; `Enter` advances or saves and `Esc` cancels.
- `Q`, `Esc`, or the app-exit key exits when no catalog dialog is open.

The app is available on Wi-Fi builds even when the board has no built-in audio
hardware. Playback starts when a default output device has been registered,
including an output supplied by a runtime-attached expansion.

Network reception and MP3 decoding feed an app-owned PCM jitter buffer. A
separate playback worker consumes that buffer at the audio device's steady
rate. Both workers continue while the resumable WebRadio session is suspended
or another foreground app is selected. Closing WebRadio cancels the network
operation through its bounded read timeout, stops both workers, and releases
the audio device. The network worker exclusively owns the HTTP request during
that shutdown.

The playback worker, not the decoder, publishes visualizer samples after each
audio write. The widgets retain only the latest played block and may drop
intermediate display frames, so buffering does not put the visualization ahead
of the audio. WebRadio uses 256 scope samples and a 256-point SIMD-eligible FFT,
grouped into 32 displayed frequency bands.

`service.signal-widgets` provides the reusable thread-safe signed-16-bit
oscilloscope and spectrum components used by the graphical player. The widgets
accept mono or interleaved multichannel PCM and own their snapshot storage;
applications retain ownership of their audio streams.

## help

Foreground browser for the package-aware SolarOS manual. The foldable tree
groups the topics compiled for the current firmware and shows whether it is
using the embedded copy or a verified downloaded revision. All groups start
folded. The selection, scroll position, and fold state remain unchanged after a
topic closes.

Usage:

```text
help
help agent
```

Controls:

- `Up`/`Down`, `Page Up`/`Page Down`, `Home`/`End`: move through the tree.
- `Left`/`Right`, `Enter`, or Space on a group: fold or unfold it.
- `Enter` or `Right` on a topic: open it in `reader` on graphic display shells
  or `less` on text shells.
- `q`, `Esc`, or the app-exit key: return to the shell.

The maintenance forms `help status`, `help update`, and `help reset` remain
shell operations. SD-capable builds show terminal-width-aware progress while
downloading and extracting one exact-version signed manual archive.

## edit

Text editor for files on mounted storage. It supports cursor navigation,
selection, clipboard operations, text-size changes, and syntax highlighting for
known source files. The editor supports files up to 256 KiB on boards with
PSRAM and 32 KiB on boards without PSRAM. Use `hexedit` for binary files.

Usage:

```text
edit <file>
```

Controls:

- Arrows move the cursor.
- `Ctrl+Left`/`Ctrl+Right` move by words.
- `Shift+Arrows` extend selection.
- `Ctrl+A`, `Ctrl+C`, `Ctrl+X`, `Ctrl+V` select all, copy, cut, and paste.
- `Ctrl+F` opens Find and `F3` jumps to the next case-insensitive match,
  wrapping at the end of the file.
- `Ctrl+S` saves in place. `Ctrl+Q`, `Esc`, or the app-exit key exits without
  saving pending changes.
- `Ctrl++` and `Ctrl+-` adjust editor text size for the active session.

## hexedit

Two-pane binary editor for files on mounted storage. Each row shows a file
offset, hexadecimal bytes, and their synchronized printable ASCII view. The
number of bytes per row adapts to the terminal width. It uses the same 256 KiB
PSRAM and 32 KiB internal-memory limits as `edit`.

Usage:

```text
hexedit <file>
```

Controls:

- `Tab` switches input focus between the HEX and ASCII panes. The corresponding
  byte remains highlighted in both panes.
- Hexadecimal digits replace the active high or low nibble in the HEX pane.
  Printable characters replace the active byte in the ASCII pane.
- Arrows, Page Up/Page Down, Home/End, and Ctrl+Home/Ctrl+End navigate by byte,
  row, page, or file.
- `Shift` with navigation extends a byte selection. `Ctrl+A`, `Ctrl+C`,
  `Ctrl+X`, and `Ctrl+V` select all, copy, cut, and paste binary data.
- Backspace and Delete remove bytes. Typing at end of file appends data.
- `Ctrl+S` saves in place. `Ctrl+Q`, `Esc`, or the app-exit key exits without
  saving pending changes.
- `Ctrl++` and `Ctrl+-` adjust editor text size for the active session.

## files

File manager inspired by Midnight Commander. Its normal mode provides two panes
for copy, move, delete, and launch workflows on mounted storage. Launcher mode
provides a minimal single-pane application menu suitable for a startup script.

Usage:

```text
files [--launcher] [path]
```

`files --launcher` hides dot-prefixed entries, removes the message and function
key bars, and expands one pane to the bottom of the terminal. Launcher mode is
read-only: navigate with the arrows, open with `Enter`, refresh with `r`, move
to the parent with Backspace, and exit with `q` or the app-exit key. To make a
directory the startup menu, add a line such as this to `/.shell/startup`:

```text
files --launcher /apps
```

File associations come from the installed app registry. Only apps compiled in
the active firmware can be selected. Associations include images to `view`,
WAV/MP3 to `player`, CSV to `sheet`, Python and Lua scripts to their runtimes,
documents to `reader` (or `writer` when Reader is unavailable), and `.gb` ROMs
to `gameboy`. Unknown files fall back to `less` or `edit`. A `.sh` file runs
through the built-in SolarOS shell. In launcher mode, documents associated with
Reader open as `reader --pager` for row-aware page navigation.

Controls:

- Arrows navigate panes.
- `Tab` switches active pane.
- `Enter` opens directories or launches known files.
- Returning to a parent directory restores the cursor to the directory that
  was just exited.
- `F5`, `F6`, and `F8` copy, move, and delete the current or marked entries.
  Copy and recursive delete scan their source trees first, then show measured
  progress with the current entry. Move shows progress across the selected
  top-level entries.
- `Esc` cancels an active copy, move, or delete operation and keeps Files open.
  A partial current-file copy is removed; top-level items completed before the
  cancellation remain copied, moved, or deleted.
- File operations refresh both panes after completion.
- App-exit key exits.

## flash

Download verified SolarOS factory artifacts to SD and program another supported
ESP board over UART. The browser refreshes the signed catalog on request and
shows which board, flavor, and version artifacts are already cached. Its tree
starts folded and retains its selection and fold state after operations. Delete
removes a selected cached artifact after confirmation. The shell form accepts a
named UART plus optional boot and reset GPIO pins.

```text
flash
flash refresh
flash list
flash download BOARD FLAVOR [VERSION]
flash BOARD FLAVOR [version=VERSION] [port=uart0] [boot=PIN] [reset=PIN] [baud=RATE]
```

See [Flash another ESP board](flash.md) for wiring, target-selection, security,
storage, and verification details.

## contacts

Provider-neutral address book for gateway and MeshCore identities. Contacts can
carry multiple provider endpoints while retaining trust independently for each
endpoint. A signed MeshCore advert creates a `discovered` endpoint: the
signature proves possession of the advertised key, not the human identity
behind it.

Open the searchable TUI:

```text
contacts
```

The list is grouped by trust and provider. Press `/` to search, `Enter` to view
the selected endpoint addresses, capabilities, last-seen times, and bounded
provider metadata summary, and `Esc` or `q` to leave.

Use the shell surface for mutations:

```text
contacts status
contacts list [all|discovered|trusted|blocked]
contacts show CONTACT_ID
contacts rename CONTACT_ID NAME
contacts trust CONTACT_ID [ENDPOINT_ID]
contacts block CONTACT_ID [ENDPOINT_ID]
contacts remove CONTACT_ID
contacts link TARGET_CONTACT_ID SOURCE_CONTACT_ID
```

Contact and endpoint identifiers autocomplete from live service snapshots.
Linking moves the source endpoints to the target contact and removes the source
record. When the 64-contact store is full, SolarOS may evict the oldest
unpinned contact whose endpoints are all still discovered; trusted and blocked
records are never automatically evicted.

The versioned store is CRC checked, uses two alternating headers and data
copies, remains below 24 KiB, and normally lives at
`/.contacts/contacts.bin`. If storage is unavailable, Contacts remains usable
in volatile mode and `contacts status` reports the storage error.

## inbox

Universal incoming-message browser for pages, chat notifications, mail, and
other background producers. It reads the same shared inbox that supplies the
status-bar unread count. Messages and read state survive reboot in the bounded
`/.inbox/messages.bin` store; the service retains at most 64 entries and keeps
the file below 32 KB even when internal flash is the only storage.

Each list item occupies exactly one terminal row: unread/priority markers,
local reception date and time, a compact source (`chat/general`, `email`, or
`pocsag`), and as much of the message body preview as fits the screen.

Notification sound is enabled by default on boards with audio. Press `s` in the Inbox or
use `inbox notify on`, `inbox notify off`, or `inbox notify test`; the setting
is persistent. Notification tones remain active while SolarOS is suspended.

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
- `s` toggles the notification sound on boards with audio output.
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

Interactive expansion I/O manager. Its default Layout view presents the
board's connectors in their physical arrangement, followed by the existing
pin, named-bus, and resource-claim views. It uses the same ownership and
validation services as the `gpio`, `i2c`, `spi`, `uart`, `midi`, `onewire`, and
`expansion` commands.

Usage:

```text
io
```

Controls:

- `Tab` switches between Layout, Pins, Buses, and Claims. Outside Layout,
  `Left` and `Right` also switch views.
- Arrows move through the physical connector grid in Layout and through rows
  in the other views. Page Up/Page Down, Home, and End make larger moves.
- Layout adapts to the terminal: compact headers show several pins across,
  while long headers such as the DevKitC J1/J3 pair scroll vertically with one
  physical pin pair per row.
- Layout markers are `*` free, `~` releasable, `@` claimed, `!` fixed/control,
  `+` power, `-` ground, and `x` not connected.
- `Enter` opens context-sensitive actions for a pin or bus.
- `n` creates a board-approved named I2C, SPI, UART, MIDI, or 1-Wire bus.
- Bus creation uses arrows to select fields and values; the generated bus name
  can be edited directly.
- Runtime buses can be attached, detached, or removed when their lease state
  permits it. Their `Autostart` action idempotently appends the matching
  `expansion bus create ...` command to `/.shell/startup`. Direct GPIO and PWM
  assignments can be created and released from the Pins or Layout view.
- A selected I2C bus has a `Set I2C speed` action with 100 kHz, 400 kHz, and
  1 MHz choices. The change applies at runtime even while devices lease the bus.
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

## gameboy

Experimental original Game Boy (DMG) emulator for the Waveshare
ESP32-S3-RLCD-4.2 and the full-PAL Freenove ESP32-WROVER v3.0 target. It is
included in the `retro` and `rover-retro` flavors. The application loads a
user-supplied ROM into PSRAM, renders its four shades as a 320x288 dithered
image, and writes battery-backed cartridge RAM beside the ROM as a `.sav`
file. Game Boy Color-only ROMs and ROMs larger than 4 MiB are rejected.

The emulator runs the core independently from the relatively expensive RLCD
update. It keeps Peanut-GB's hot state and up to two 16 KiB ROM banks in
internal RAM when the SolarOS reserve permits, skips alternate core-rendered
frames by default, and presents the newest frame once per three emulated
frames. On the Waveshare display, a dedicated monochrome presentation path
rotates and streams the frame in one controller write sequence. On Rover, a
direct byte-aligned conversion copies XBM rows into the inactive PAL scanout
buffer and swaps it at a field boundary; the 320x288 image is centered in the
384x288 canvas and reaches the top and bottom edges. Runtime logs report
emulation and presentation rates separately.

Audio rendering runs in its own bounded worker and holds exclusive speaker
output while Game Boy is active. RLCD presentation runs independently at about
20 Hz, temporarily requests the panel's 25.5 Hz HPM profile, and drops stale
frames instead of blocking emulation. Pausing, suspending, or exiting the app
stops the synth and restores the previous display and audio policies.
These audio and HPM behaviors apply to the Waveshare `retro` build. The Rover
build deliberately compiles Game Boy without sound because PAL scanout owns
I2S0. Its 320x200 composite safe-area mode is too short for the 320x288 Game
Boy canvas and is not supported by `rover-retro`.

Usage:

```text
gameboy <file.gb>
```

Controls:

- Arrows control the D-pad.
- The physical US-Z key position is A; on a German QWERTZ keyboard this key is
  labeled `Y`. The physical X key is B.
- `Enter` is Start; `Backspace` or `Delete` is Select.
- `p` pauses and `r` resets.
- `q`, `Esc`, or the app-exit key exits.

Game Boy reads the generic SolarOS held-key state. BLE/PS/2 keyboards, fixed board
buttons, `gpio-keys`, and ADC D-pads therefore remain active until
release, and combinations such as diagonal movement or direction plus A/B work
simultaneously. USB HID usages preserve the physical Z/X positions across
keyboard layouts. Character-only inputs such as port-shell input retain the
short button-pulse fallback. Analog joystick axes do not become Game Boy keys.

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
- `Ctrl+F` or `/` opens Find. `F3` or `n` jumps to the next
  case-insensitive match; `N` jumps to the previous match. Search wraps.
- `q`, `Esc`, or app-exit key exits.

## logic

On-device logic analyzer waveform viewer. It displays the latest capture made
by the shared logic analyzer service or the SUMP job. With pin arguments it
makes a new local capture before opening the viewer.

Usage:

```text
logic
logic <pin[,pin...]> [rate-hz] [samples] [trigger=<pin>]
```

Examples:

```text
logic
logic 1,2,3,17
logic 1,2 500000 8192
logic 1,2,3,17 10000 4096 trigger=1
```

`trigger=<pin>` waits for the next rising or falling edge on that runtime-safe
GPIO before sampling. The trigger pin may be one of the captured data pins, as
in the last example, or a separate GPIO that is not displayed as a channel.

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
firmware includes the corresponding packages. Foreground scripts can consume
touch coordinates, relative mouse motion, buttons, and joystick axes through
`solaros.input`.

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
items and supports one level of category folding. A persistent bottom help bar
shows the available controls, with status or text input directly above it.

Usage:

```text
notes [file.md]
```

Controls:

- `Up`/`Down` navigate.
- `Space` toggles an item. After an active item is marked done, the selection
  remains on the next active item when one is available.
- `a` adds an unchecked item below the selected item. On a category, it adds the
  first item in that category; from the done section, it adds at the end of the
  active items.
- `c` adds a category.
- `Enter` edits the selected line.
- `d` or `Delete` deletes the selected item/category.
- `t` tidies the note by deleting all completed items.
- `Shift+Up`/`Shift+Down` reorders items within a category or moves the selected
  category together with all of its items.
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

`--rate` is a best-effort live sampling interval in milliseconds. Rates below
25 ms request faster runtime ticks while the screen redraws no more often than
once every 25 ms, so fast acquisition does not force an equally fast display
refresh.

Controls:

- `Left`/`Right` pan.
- `Up`/`Down` select series.
- `+`/`-` or `Page Up`/`Page Down` adjust visible window/zoom.
- `a` or `r` resets the view.
- `Space` pauses/resumes live mode.
- `q`, `Esc`, or app-exit key exits.

## playground

Browse the configured community catalog as a foldable category tree, search
applications, and install, update, uninstall, or run Python and Lua scripts.

Usage:

```text
playground
playground search QUERY...
playground install APP-ID [auto|flash|sd]
playground run APP-ID [ARG...]
playground delete
playground refresh
playground reload
playground source [repository-or-catalog-url|reset]
playground storage [flash|sd]
```

Press `/` to search, `Enter` to open an application, and use the actions shown
in the bottom bar. Press `i` on an application in the catalog tree or details
page to install it, and `u` to uninstall it after confirmation. Packages are
verified by size and SHA-256 and installed under `/playground/` on the
permanently configured `flash` or `sd` storage. Community scripts run with the
normal permissions of their runtime and are not sandboxed.

Catalog and application files are stored under `/playground/` on the selected
filesystem. `playground delete` recursively removes that entire directory,
clears the loaded catalog from memory, and also removes the legacy hidden
`.solar/playground` directory when present. Source and storage preferences are
retained.

The shell subcommands use the local catalog for browsing and installation:
`refresh` downloads and saves it, `reload` loads that saved copy without network
access, `search` prints matches, and `install` downloads and verifies an
application by ID. `run` reads an installed application's own manifest, so it
does not require a catalog reload. It launches the declared Python or Lua
runtime without creating a Playground session. Installation also adds the app
ID to the managed `/.shell/playground` aliases, so `qr-share --file
/notes/wifi.txt` is equivalent to `playground run qr-share --file
/notes/wifi.txt`. Uninstalling removes the generated alias. Arguments are
forwarded unchanged to the selected script. Opening the TUI reloads the saved
catalog automatically and does not refresh it. At the top-level catalog tree,
`Esc`, `q`, and the app-exit key exit Playground.

`playground storage` shows the persistent catalog and default application
storage. Set it with `playground storage flash` or `playground storage sd`.
Without a saved preference, Playground selects SD when it is mounted and flash
otherwise. Omitting the `install` target, or specifying `auto`, uses the
selected setting.

See [Playground](playground.md) for controls, storage layout, source selection,
and the trust model.

## python

Embedded MicroPython runtime. It can run an interactive REPL, `.py` scripts, or
`.mpy` files from storage. Python scripts can use SolarOS service bindings when
the selected firmware includes the corresponding packages. Foreground scripts
can consume touch coordinates, relative mouse motion, buttons, and joystick
axes through `solaros.input`.

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
reader [--pager] <file.txt|file.md|file.epub|man:topic>
```

With `--pager`, `Up` and `Down` page instead of scrolling one layout row. Each
forward page starts with the final visible row from the previous page, so
mixed font sizes and wrapped Markdown retain a precise reading overlap.

Controls:

- `Up`/`Down` scroll by layout row, or page when `--pager` is active.
- `Page Up`/`Page Down` page with the same precise row overlap.
- `Home`/`End` jump to start/end.
- `+`/`-` adjust zoom.
- `Ctrl+F` or `/` opens Find. `F3` or `n` jumps to the next
  case-insensitive match; `N` jumps to the previous match. Search wraps.
- `Esc` exits search state first; otherwise exits.
- `q` or app-exit key exits.

## writer

Resumable graphical Markdown editor for PSRAM display boards. Inactive blocks
are formatted like `reader`; the block containing the cursor and every block
touched by a selection show their exact Markdown source. `edit` remains the
portable text editor for port shells and boards without graphics or PSRAM.

Usage:

```text
writer [file.md]
```

Without a path, Writer opens an untitled document and asks for a path on the
first save. Existing files larger than 256 KiB are rejected without changing
them. Saves use a synced same-directory staging file, backup rename, verified
replacement, and rollback. Cursor, scroll anchor, and zoom metadata plus idle
recovery snapshots live centrally under `/.writer` or the active persistent
storage root. Writer offers a differing recovery snapshot when the document is
opened again. Recovery uses `R` to recover, `D` to discard the snapshot, or `C`
to cancel; the unsaved-changes dialog similarly uses `S`, `D`, and `C`.

Controls:

- Left/Right move by UTF-8 codepoint; Up/Down move to the adjacent editable
  visual line or Markdown block, including directly adjacent headings.
  `Ctrl+Left`/`Ctrl+Right` move by word. The insertion cursor blinks while idle.
- `Shift` with arrows, Page Up/Page Down, Home/End, or document movement extends
  the selection. `Ctrl+Home`/`Ctrl+End` jump to the document boundaries.
- `Ctrl+A`, `Ctrl+C`, `Ctrl+X`, and `Ctrl+V` select all, copy, cut, and paste
  through the shared SolarOS clipboard.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` add or remove bold, italic, and link source.
- `F1` opens formatting for inline code, headings 1–4, bullet or numbered
  lists, quotes, fenced code, and rules. `Esc` closes the active menu/dialog;
  from the editor it exits, using the save/discard/cancel prompt when dirty.
- `Ctrl+F` opens Find and `F3` jumps to the next case-insensitive match,
  wrapping at the end. `Ctrl+R` prompts for find and replacement text.
- `Ctrl+S` saves, `Ctrl+Z`/`Ctrl+Y` undo and redo, and `Ctrl++`/`Ctrl+-` adjust
  zoom.
- `Esc` and the app-exit key open the save/discard/cancel prompt when the
  document is dirty. Writer writes recovery before suspend or stop and resumes
  in place.

## scp

SCP file transfer over SSH. It supports password or key authentication through
the shared SSH transport and host lookup/known-host storage. When `user@` is
omitted, SCP uses the NVS-backed SolarOS identity user.
Tab completion reads aliases from `/.ssh/hosts`, preserves an explicit `user@`
prefix, and appends `:` after a unique host match.

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

SCP runs inline in the shell. Its progress and result remain in the terminal
scrollback, and usage errors or a completed transfer return directly to the
prompt.

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
remote full-screen terminal applications. When `user@` is omitted, SSH uses the
NVS-backed SolarOS identity user.
Tab completion reads aliases from `/.ssh/hosts` and preserves an explicit
`user@` prefix.

Usage:

```text
ssh [user@]host [port]
```

On display shells, SSH owns a resumable terminal buffer. Switching to the
SolarOS shell does not mix the local and remote scrollback, and switching back
restores the SSH buffer. On UART and USB CDC port shells, SSH uses the shared
port scrollback. Disconnecting returns directly to the local prompt. Remote
terminal control sequences still work, including full-screen applications.

Controls:

- Most keys are sent to the remote host, including Esc, Alt+key, cursor keys,
  Ctrl combinations, and function keys.
- App-exit key closes the SSH app.
- `Alt+Tab` leaves the session running in the background on display builds.

## synth

Open the native synthesizer and sound designer:

```text
synth [--headless]
```

The app uses the same eight-voice fixed-point engine exposed to Python and Lua,
but keyboard and MIDI events reach it directly. On a display, the default
graphical interface provides the complete editor described below. `--headless`
suppresses graphical rendering while keeping the same note keys, editing keys,
published parameters, control bindings, and MIDI input active. This permits use
from a port shell on a headless board after a playback device such as the LEDC
PWM audio expansion has been attached. Its Play tab contains the waveform,
envelope, volume, ADSR, and piano controls. The Wave tab is a graphical
wavetable editor with selectable 16, 32, or 64-point resolution;
edits reshape held notes immediately while the piano remains playable, and
switching tabs does not rewrite the custom wavetable. The
Filter tab adds a resonant low-pass response graph, cutoff, resonance, envelope
amount, and an independent graphical ADSR filter envelope. The Oscillator 2 tab
adds a second per-note source with waveform, octave, fine detune, and unity-safe
mix controls. Both oscillators share the filter and envelopes. The Preset tab
provides eight factory sounds and eight persistent user slots. User presets
capture both oscillators, both envelopes, the filter, mono/poly mode, glide,
and the complete custom wavetable. The Glide tab provides polyphonic or
monophonic last-note playback, a hold mode for toggle-style piano keys, and
portamento. Hold is session performance state and is not part of a preset. The
display also reports active voices, output sample rate, and audio errors. The
volume button changes the shared SolarOS speaker volume.

On display targets smaller than 240 pixels wide or 200 pixels high, Synth
automatically replaces the full editor with a parameter HUD. The selected
control gets a large value and level bar; waveform controls and the Wave tab
retain compact graphs; Presets shows one slot at a time. Targets smaller than
112 by 56 pixels use a footer-free micro layout. Physical note keys and MIDI
remain active. External parameter/control changes focus the changed parameter
until the next local navigation action, which makes a 128-by-64 SH1106 or
SSD1306 useful as a synthesizer appliance display. After attaching it as
`oled0`, open Synth on that target with `session create synth oled0`.
During active playing, compact displays defer visualization until note input is
quiet so synchronous display transfers cannot take priority over new notes.
MIDI notes highlight the matching pitch class on the on-screen piano just like
physical note keys.

Controls:

- `A W S E D F T G Y/Z H U J K` play one chromatic octave. The positions are
  physical, so the `Y` position is the `Z` key on a German keyboard. Note input
  remains active on all tabs.
- `Tab` cycles through Play, Filter, Wave, Oscillator 2, Glide, and Presets.
  Number keys `1` through `6` select those tabs in that order.
- `X` hides or shows the on-screen piano keyboard on every tab. When it is
  hidden, the tab's knobs, graphs, panels, or preset list use the freed space;
  the physical note keys and MIDI input remain active.
- On Glide, select Hold and use `Up` or `Enter` to enable it. Each physical or
  terminal piano-key press then toggles its note on or off, and releasing the
  key does not stop the note. Use `Down` or `Enter` to disable hold and release
  all latched app-key notes. MIDI Note On, Note Off, and sustain keep their
  normal behavior.
- On Play, `Left`/`Right` selects the waveform, global volume, or an ADSR knob;
  `Up`/`Down` changes it, and `+`/`-` changes note velocity.
- On Wave, `Left`/`Right` moves the edit cursor and `Up`/`Down` changes the
  waveform. Shifted arrows move or draw faster, and `+`/`-` changes brush size.
- On Wave, `Enter` cycles the editor through 16, 32, and 64 points. New sessions
  start at 16 points.
  The current waveform is resampled rather than reset.
- On Wave, `B` applies the next square, triangle, saw, Supersaw, sine, or flat
  baseline; `R` restores that baseline, `M` smooths, `N` normalizes, `0`
  clears, and `Backspace` or `Delete` exchanges the current waveform with the
  undo state.
- On Filter, `Left`/`Right` selects cutoff, resonance, envelope amount, or a
  filter ADSR knob; `Up`/`Down` changes it, and `+`/`-` changes note velocity.
- On Oscillator 2, `Left`/`Right` selects waveform, octave, fine detune, or mix;
  `Up`/`Down` changes it, and `+`/`-` changes note velocity. Octave ranges from
  -2 through +2, detune from -100 through +100 cents, and mix from 0 through
  100 percent.
- On Preset, arrows select a factory or user slot and `Enter` loads it. `V`
  saves the current sound to the selected user slot. Factory presets are
  read-only.
- `Page Up`/`Page Down` changes octave from 2 through 6 on any tab.
- `Esc` or the app-exit key exits and immediately releases audio ownership.

For an external MIDI keyboard, create and start a named MIDI bus before
opening Synth:

```text
expansion bus create midi midi0 tx=gpio1 rx=gpio2
job start midi midi0
synth
```

MIDI Note On/Off, velocity, sustain (CC64), All Sound Off (CC120), and All
Notes Off (CC123) are supported on all 16 channels.

### Published parameters

While Synth is running, `control parameters` exposes these native continuous
parameters:

- `synth.volume`: 0 through 100 percent, linear.
- `synth.envelope.attack`: 0 through 10000 ms, linear.
- `synth.envelope.decay`: 0 through 10000 ms, linear.
- `synth.envelope.sustain`: 0 through 100 percent, linear.
- `synth.envelope.release`: 0 through 10000 ms, linear.
- `synth.filter.cutoff`: 40 through 18000 Hz, logarithmic.
- `synth.filter.resonance`: 0 through 100 percent, linear.
- `synth.filter.envelope.amount`: 0 through 100 percent, linear.
- `synth.filter.envelope.attack`: 0 through 10000 ms, linear.
- `synth.filter.envelope.decay`: 0 through 10000 ms, linear.
- `synth.filter.envelope.sustain`: 0 through 100 percent, linear.
- `synth.filter.envelope.release`: 0 through 10000 ms, linear.
- `synth.osc2.octave`: -2 through +2 octaves, linear.
- `synth.osc2.detune`: -100 through +100 cents, linear.
- `synth.osc2.mix`: 0 through 100 percent, linear.
- `synth.glide`: 0 through 2500 ms, linear.

These paths exist only while the app is active. Bindings remain configured
when Synth is suspended or stopped and reconnect when it resumes. See
`man controls` for control creation, soft pickup, and MIDI CC mappings.

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
playback when the media package is enabled. Images are decoded as RGB on a
negotiated indexed-color display and as grayscale on a one-bit display. In the
default fit mode, JPEG color conversion writes display-sized output directly,
so a large source photograph does not require a full-size RGB destination.

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

## sketch

Pointer-driven graphical paint application. Its layout follows classic desktop
paint programs: Save, Open, Import, and the sidebar controls share one aligned,
equal-sized button grid; color and pattern choices are
in the bottom bar. Sketch uses a compact four-color canvas and stores finished
documents as interoperable indexed-color PNG files. Color TFTs show the native
palette; one-bit displays use the existing dithered rendering path.

Usage:

```text
sketch
sketch file.png
```

Sketch is intentionally not gated on a pointer hardware capability. If no ready
absolute or relative pointer is registered when the app starts, a popup explains
that a pointer can be attached at any time. Press Enter or Esc to dismiss it;
the first attached pointer is accepted without restarting the app.

Controls:

- Click Save to update the current PNG. For an untitled or imported image, the
  first save selects the next free `Sketches/sketchNNN.png` path on persistent
  storage. Saves use a synced staging file, backup rename, atomic replacement,
  and rollback.
- Click Open to select a PNG as the current document. Click Import to flatten a
  PNG, JPEG, or GIF into the current four-color canvas; the next save creates a
  new PNG instead of overwriting the imported source.
- The sidebar selects pen, straight-line, rectangle, ellipse, bucket fill,
  or eraser drawing. The weight control cycles through 1, 2, 4, and 8 pixels.
  Eraser uses the selected weight and restores white pixels. Clear immediately
  resets the complete canvas to white.
- Line, rectangle, and ellipse show an exact non-destructive preview during the
  drag, including the selected color, pattern, and stroke weight. Releasing the
  pointer commits that preview to the canvas.
- The first four bottom swatches select white, red, blue, or black. The next
  four select solid, checker, dot, or diagonal-hatch application.
- Keyboard fallbacks are `S` Save, `O` Open, `I` Import, `P` Pen, `L` Line,
  `R` Rectangle, `E` Ellipse, `B` bucket fill, `X` eraser, and `C` clear.
  Arrows and Enter operate the file browser.
- `Q`, Esc, or the app-exit key exits. Suspending and resuming retains the
  current cold-allocated canvas; closing the app releases it and all browser or
  image resources.

## web

Simple graphical web browser for lightweight HTML pages. It shares document and
image rendering infrastructure with `reader` where possible. Embedded and
direct PNG, JPEG, GIF, and WebP images retain color on indexed-color displays;
one-bit displays keep the grayscale decode and dither path.

Usage:

```text
web http://host/
web https://host/path
```

Controls:

- Keyboard navigation follows the active web UI state.
- `Ctrl+F` opens Find and `F3` jumps to the next case-insensitive match in
  rendered page text, wrapping at the end.
- `Esc` or app-exit key exits.

## Quick reference

Use `apps` to list applications installed in the current firmware. Start an app
by entering its name and arguments. Use the app-exit key to return to the shell;
resumable applications can also be switched through sessions. This page is the
complete usage and controls reference for foreground applications.
