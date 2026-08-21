+++
id = "commands"
title = "Shell command reference"
section = "shell"
summary = "Complete syntax, behavior, and examples for built-in shell commands"
aliases = ["command", "shell"]
keywords = "shell commands syntax examples files network hardware ota sessions jobs"
packages_any = []
+++
# SolarOS Shell Commands

This document covers built-in shell commands. Foreground applications are
documented separately in [apps.md](apps.md). Command availability depends on the
compiled firmware flavor and board capabilities; `commands` shows the commands in
the running image.

## Shell Conventions

Paths are resolved relative to the current shell directory. The default storage
volume is presented as `/`. On SD-backed boards, the primary SD card volume also
has the internal mount path `/sdcard`, and the internal flash volume is mounted
at `/flash`. On boards without SD support, internal flash is mounted as `/`.

Wildcard patterns are supported by selected filesystem commands, for example
`*.txt`.

Tab completion covers commands, subcommands, filesystem paths, job names, port
names, and stream IDs where the command exposes enough structure.

`Ctrl+V` pastes the shared SolarOS clipboard at the command-line cursor. Line
breaks and tabs become spaces, and a paste stops at the shell input limit; it
never executes a command by itself.

Invalid input is reported as a specific problem followed by only the relevant
usage line. Close, unambiguous command and subcommand typos include a `did you
mean` hint; SolarOS never runs the suggested command automatically. Missing,
unexpected, and invalid arguments identify the affected argument and its
expected form. Passwords, tokens, and other credential values are redacted.

Quotes and backslash escapes are checked before a command runs. Unterminated
quotes, a trailing backslash, too many arguments, and unsupported shell
operators such as `|`, `>`, `&&`, and `;` reject the complete line. URLs and
ordinary argument text containing punctuation remain valid.

Shell scripts use the `.sh` extension. Run one with `sh <file>` or invoke its
path directly; for example, `./somescript.sh` is equivalent to
`sh ./somescript.sh`. SolarOS storage does not require an executable permission
bit for this shorthand.

History is kept in memory and cached at `/.shell/history` when storage is
available. The optional alias file follows the default storage volume:

```text
/.shell/alias
```

The startup script source is selected with `setterm startup [flash|sd]` and is
stored in NVS. Internal flash is the default, including after `nvs clear`. On an
SD-capable board, the paths are `/flash/.shell/startup` and
`/sdcard/.shell/startup`; on a board without SD, internal flash is mounted at
`/`, so its path is `/.shell/startup`. The selected source does not fall back to
the other volume when it is unavailable. The script runs once per boot on the
first startup-enabled shell. Shell sessions created by that script do not run
it again.

The device user and hostname are stored in NVS and configured with `identity`.
The user is also the default remote username used by `ssh` and `scp` when
`user@host` is not supplied.
On the first boot after upgrading, existing `/.solar/user` and
`/.solar/hostname` values are imported when the corresponding NVS value is not
already set.

The display-shell app exit chord is `CTRL+ALT+DEL`. Port shells use `Ctrl+]`.

## Shell Control

| Command | Usage | Description |
| --- | --- | --- |
| `commands` | `commands` | List built-in shell commands. |
| `help` | `help [TOPIC]`; `help status`; `help update`; `help reset` | Browse the package-aware manual or manage its signed exact-version SD copy. |
| `man` | `man TOPIC`; `man -k QUERY...`; `man --list` | Read or search the package-aware SolarOS manual. |
| `clear` | `clear` | Clear the active shell terminal. |
| `watch` | `watch [-n seconds] <command> [args...]` | Repeat another shell command until `Esc`, `q`, or the app-exit key is pressed. |
| `sh` | `sh <file>` | Run a simple SolarOS shell script from storage. |
| `exit` | `exit` | Close the current UART, USB CDC, or telnet shell when another interactive shell remains. |
| `reboot` | `reboot` | Restart the board. |
| `nvs` | `nvs status` | Show the default NVS partition size, entry usage, and namespace count. |
| `nvs` | `nvs backup [file]` | Back up the complete NVS partition to disk. The default is `/.solar/nvs.bin`. |
| `nvs` | `nvs restore [file]` | Validate and restore a complete NVS backup, then reboot. The default is `/.solar/nvs.bin`. |
| `nvs` | `nvs clear` | Erase all NVS-backed settings and reboot immediately. |
| `sessions` | `sessions` | List display app sessions, display shell sessions, and port shell sessions. |
| `fg` | `fg [session-id]` | Resume a display session or a port-owned app on its owning terminal. Without an ID, restore the calling port shell's most recently suspended app. |
| `close` | `close <session-id>` | Close a display app, display shell, or retained port app, or stop a port shell session. The final interactive shell cannot be closed. |
| `inbox` | `inbox` | Open the universal incoming-message browser. |
| `inbox` | `inbox status` | Show universal incoming-message counts and storage status. |
| `inbox` | `inbox list [all\|unread]` | List newest messages first. |
| `inbox` | `inbox read <id>` | Print one message and mark it read. |
| `inbox` | `inbox delete <id>` | Delete one message by its decimal ID. |
| `inbox` | `inbox clear` | Remove every message. |
| `inbox` | `inbox post <source> <message>` | Post a message from a shell script or for testing. |
| `inbox` | `inbox notify [on\|off\|test]` | Show, persist, disable, or test the Inbox notification sound. It defaults to on and is unavailable on boards without audio output. |
| `contacts` | `contacts` | Open the searchable provider-neutral contact browser. |
| `contacts` | `contacts status` | Show contact, endpoint, persistence, PSRAM, and opaque-credential counts. |
| `contacts` | `contacts list [all\|discovered\|trusted\|blocked]` | List contacts, optionally filtered by endpoint trust. |
| `contacts` | `contacts show <contact-id>` | Show a contact and its bounded provider endpoints. |
| `contacts` | `contacts rename <contact-id> <name>` | Change the local contact display name. |
| `contacts` | `contacts trust <contact-id> [endpoint-id]` | Trust one endpoint or every endpoint on a contact. |
| `contacts` | `contacts block <contact-id> [endpoint-id]` | Block one endpoint or every endpoint on a contact. |
| `contacts` | `contacts remove <contact-id>` | Remove a contact and all its endpoints. |
| `contacts` | `contacts link <target-contact-id> <source-contact-id>` | Move the source endpoints into the target and remove the source contact. |
| `messages` | `messages status` | Show bounded-store, persistence, drop, and live provider state. |
| `messages` | `messages conversations` | List provider-neutral conversations and unread/security state. |
| `messages` | `messages list <conversation-id>` | List retained messages and their stable hexadecimal IDs. |
| `messages` | `messages send <conversation-id> <text> [--allow-untrusted]` | Queue an outbound message; discovered direct endpoints require the explicit flag. |
| `messages` | `messages read <conversation-id>` | Mark a conversation and its linked Inbox entries read. |
| `messages` | `messages delete <message-id>` | Delete one retained message and its linked Inbox projection by hexadecimal ID. |
| `messages` | `messages clear <gateway\|meshcore\|link\|all>` | Clear retained history and owned Inbox projections for one provider or every messaging provider. Unrelated Inbox sources remain. |
| `messages` | `messages outbox` | List pending outbound requests in queue order. |
| `messages` | `messages cancel <message-id>` | Cancel a queued outbound message by the hexadecimal ID printed by `list` or `send`. |
| `outbox` | `outbox [list]` | List pending outbound messages. Sent and failed messages remain in conversation history, not Outbox. |
| `outbox` | `outbox cancel <message-id>` | Cancel one pending message by hexadecimal ID. |
| `gateway` | `gateway status` | Show gateway configuration, connection state, and traffic counters. |
| `gateway` | `gateway configure <url> [token]` | Save gateway connection settings. The gateway uses the global SolarOS user identity. |
| `gateway` | `gateway connect [url] [token]` | Enable gateway synchronization, optionally updating settings. |
| `gateway` | `gateway disconnect` | Disable gateway synchronization. |
| `gateway` | `gateway rooms` | List known and joined gateway rooms. |
| `gateway` | `gateway join\|leave\|delete <room>` | Queue a gateway-specific room operation. |
| `email` | `email` | Open the receive-only email app. |
| `email` | `email status` | Show saved account, local message counts, and last sync error. |
| `email` | `email configure <imaps://host[:port]> <user> <password> [mailbox]` | Save an IMAPS account; the default mailbox is `INBOX`. |
| `email` | `email sync` | Start a one-shot mailbox synchronization. |
| `email` | `email forget` | Remove the saved account and local email list. |
| `pocsag` | `pocsag status` | Show POCSAG receiver configuration, counters, correction statistics, and RSSI. |
| `pocsag` | `pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha\|numeric] [normal\|inverted] [function]` | Encode and transmit one POCSAG page. |

`nvs status` distinguishes raw free entries from entries currently available
for new data; use the available count when diagnosing a failed NVS write.
`nvs backup` writes a versioned, CRC-protected image of the complete default NVS
partition. The file contains unencrypted credentials and settings, so protect
it like a password. `nvs restore` accepts only a complete backup for the current
NVS partition address and size, verifies its CRC before changing flash, verifies
the written partition again, and reboots. `nvs clear` erases the complete
default NVS partition, including identity, Wi-Fi and BLE state, credentials,
service settings, and radio profiles, then reboots. Files on SD or the internal
FAT filesystem are not affected.

Sessions are foreground application state plus shell instances attached to a
display target or byte-stream port. Background services such as log followers,
SLIP, DAQ, and HTTP serving are jobs and are controlled with `job`.

Scripts are intentionally simple. `sh` skips blank lines and lines whose first
non-space character is `#`, then executes each remaining line as a normal shell
command. Diagnostics produced through the common command parser include the
script path and line number. A failed or malformed command does not execute, and
the script continues with its next line; `exit` stops the script and closes its
port shell. There are no variables, pipes, redirects, or conditionals yet.

`man TOPIC` opens one manual entry in the `less` pager when that app is
installed. Use `q`, `Esc`, or the app-exit key to return to the shell. `man -k`
searches page names, aliases, summaries, keywords, and API contracts; `man
--list` shows every entry compiled into the current flavor. Optional topics are
omitted when their package is absent. The same generated registry supplies the
agent's `solaros_reference` tool, so local help and generated-code guidance do
not drift apart.

Bare `help` opens a foldable topic tree. Graphic display shells read the
selected topic in `reader`; text shells use `less`, both through the same
`man:TOPIC` source. On builds with Wi-Fi, PSRAM, and SD, `help update` shows
terminal-width-aware progress while downloading one `manual.zip` published for
the exact running firmware version. The catalog signature authenticates the
archive hash; after extraction every Markdown page is checked by size and
SHA-256 before activation. `help reset` returns immediately to the embedded
manual.

Aliases are stored in `/.shell/alias`, one per line:

```text
name command-or-app fixed-args...
```

Arguments typed after the alias are appended.
Tab completion expands the complete fixed alias target. For example,
`run playground run` completes installed application IDs after `run `.

The inbox is a persistent, producer-neutral message sink. Radio decoders,
background chat or mail jobs, and shell scripts publish messages with a source,
optional topic/sender/title, priority, timestamp, and body. New messages are
unread by default. The status bar shows an envelope and unread count; reading or
clearing messages durably updates that count. The newest 64 messages are kept in
PSRAM when available and mirrored by a fixed-size ring at
`/.inbox/messages.bin`; its compiled maximum is below 32 KB, so systems using
the 64 KB internal flash volume cannot grow the inbox without bound. Replayed
mail and chat notifications retain their existing read state. The browser shows
newest messages first; opening a message marks the shared entry read.

Email configuration is saved in NVS and deliberately has no compiled remote
server or account default. Only `imaps://` endpoints are accepted, with TLS
certificate validation enabled. Use a provider-specific app password where
available. The password is supplied as a shell argument and stored with the
device configuration, so treat shell history and physical access to the device
as sensitive. `email sync` performs one synchronization; use the `email-sync`
job for periodic polling.

## System And Diagnostics

| Command | Usage | Description |
| --- | --- | --- |
| `version` | `version` | Print the SolarOS version and firmware flavor. |
| `pkg` | `pkg` | Print compiled package groups and build units. |
| `board` | `board` | Print board ID, name, and capabilities. |
| `identity` | `identity [status]` | Show the configured user and hostname. |
| `identity` | `identity user <name>` | Save the SolarOS user and default SSH/SCP username in NVS. |
| `identity` | `identity hostname <name>` | Save the device hostname in NVS; reboot to update Wi-Fi. |
| `engine` | `engine [status|reset]` | Print or reset generic engine utilization counters for CPU/SIMD-style backends and vector bulk operations. |
| `display` | `display [list]`; `display test <target>`; `display mode <target> [mode]` | List drawable display targets, draw a test pattern, or change driver-specific display settings. |
| `status` | `status` | Print a compact system status summary. |
| `uptime` | `uptime` | Print elapsed time since boot. |
| `mem` | `mem [policy]` | Print heap status; `policy` also shows allocation-class counters, guarded fallback limits, and the last tagged failure. |
| `top` | `top` | Print FreeRTOS task resource information when available. |
| `sleep` | `sleep` | Enter explicit light sleep. |
| `suspend` | `suspend` | Turn off the primary display and temporarily use the `lowpower` profile while services and jobs continue. Press KEY to resume. |
| `power` | See below | Inspect and configure power policy. |
| `setterm` | See below | Configure terminal/input preferences. Without arguments, opens the display TUI when available. |

`power` usage:

```text
power status
power profile [performance|balanced|battery|lowpower]
power idle [off|seconds]
power key [off|sleep|suspend]
power sleep
power suspend
```

Profiles:

| Profile | Behavior |
| --- | --- |
| `performance` | CPU fixed at 240 MHz, no automatic light sleep. This is the default. |
| `balanced` | CPU fixed at 160 MHz, no automatic light sleep. |
| `battery` | CPU fixed at 160 MHz with ESP-IDF automatic light sleep. |
| `lowpower` | CPU fixed at 80 MHz with automatic light sleep and display-shell idle sleep after 60 seconds. |

Suspend is different from explicit light sleep. It keeps the runtime, radios,
background jobs, Inbox notifications, and audio active while the primary
display is off. It temporarily uses the `lowpower` profile and prevents the
idle policy from entering explicit light sleep. Another short press of KEY
resumes the display and restores the selected profile. `power status` shows
the selected profile, effective profile, and suspend state.

`power key` retains `off` for compatibility. `sleep` uses the existing light
sleep path, and `suspend` toggles the runtime suspend state. The default for a
new or cleared NVS configuration is `suspend`; an existing saved value remains
unchanged.

`setterm` usage:

```text
setterm
setterm orientation [0|90|180|270]
setterm font [mono|compact]
setterm textsize [10|12|14|16|18|20]
setterm palette [normal|inverted]
setterm statusbar [show|hide]
setterm brightness [0..100]
setterm backlight [0..100]
setterm profile [vt100|ansi|dumb]
setterm charset [utf8|ascii]
setterm keyboard [us|de]
setterm powerkey [sleep|suspend]
setterm keyrate [off|1..60 [delay-ms]]
setterm timezone [UTC|Europe/Berlin|POSIX-TZ]
setterm startup [flash|sd]
setterm otaurl [url]
```

`setterm keyrate` configures the shared repeat policy for BLE keyboards, fixed
board buttons, `gpio-keys`, joysticks, ADC D-pads, and future keyboard buses.
The value is stored in NVS and is available on builds without BLE.

`setterm powerkey` selects the dedicated KEY short-press action. `sleep`
enters explicit light sleep; `suspend` turns off the display while jobs and
services continue. `setterm key` is accepted as a shorter alias.

`setterm startup` selects the volume used for `.shell/startup` on the next boot.
The default is `flash`. Selecting `sd` is rejected on boards without SD support.
Use `setterm startup` without a value to show the selected source and resolved
path.

`setterm profile` and `setterm charset` are runtime-only and apply to the
current port shell. From the display shell they print guidance to configure
them from a port shell. `profile` controls terminal escape sequences;
`charset` controls TUI glyph output. The default `utf8` mode uses Unicode box
drawing. Select `ascii` for DOS and other legacy serial terminals; TUI borders,
blocks, arrows, and punctuation are replaced with readable ASCII characters.
Display layout settings (`orientation`, `font`, `textsize`, `palette`, and
`statusbar`) apply
to the current display and its app sessions. Settings on the primary display
are persistent; settings on secondary or virtual displays such as `web0` are
runtime-only. When no saved values exist, the font defaults to `compact` and
the text size defaults to `16`. `palette` exchanges logical black and white in
terminal content
and in the shared graphics palette; dithered shades are reversed as well. It
remains independent of hardware inversion modes exposed by `display mode`, and
does not rewrite an existing framebuffer. On a headless board, a port shell can
set or query the persistent palette before an expansion-display session exists;
subsequently created terminal and graphic sessions inherit it.

`setterm statusbar hide` removes the top status bar from graphical shell
sessions and gives its space to the terminal. `show` restores it. The default is
`show` when no value is stored in NVS.

## Apps And Jobs

| Command | Usage | Description |
| --- | --- | --- |
| `apps` | `apps` | List registered foreground apps compiled into the firmware. |
| `agent` | `agent`; `agent new`; `agent ask PROMPT...` | Open a new native LLM agent TUI or make one unsaved foreground request. |
| `agent` | `agent list`; `agent resume SLOT`; `agent delete SLOT` | List, restore, or delete durable local conversation slots. |
| `agent` | `agent status`; `agent tools` | Inspect provider state, request statistics, typed tools, risk, and policy. |
| `agent` | `agent config endpoint|model|key|reasoning|tools|max-tools VALUE` | Configure the provider and tool policy. |
| `agent` | `agent script python\|lua (-c SOURCE \| FILE) [ARGS...]` | Run a bounded script through the agent execution path. |
| `agent` | `agent forget` | Erase the saved agent configuration. |
| `jobs` | `jobs` | List registered jobs and their state. |
| `job` | `job status [name]` | Show one job or all jobs. |
| `job` | `job start <name> [args...]` | Start or restart a job. |
| `job` | `job stop <name>` | Stop a job. |
| `session` | `session list` | List display sessions, port shells, and retained port-owned application sessions with their owner. |
| `session` | `session create shell <port> [--term auto|vt100|ansi|dumb] [--charset utf8|ascii] [--size COLSxROWS]` | Start a shell session on a byte-stream port. |
| `session` | `session create shell <display-target>` | Attach a shell session to a ready display target such as `lcd0`. |
| `session` | `session create <app> <display-target> [args...]` | Start a foreground application as the active session on a named display. |
| `session` | `session focus [display-target]` | Show or assign the display that receives BLE keyboard and local board-control input. |
| `session` | `session fg [id]` or `session switch [id]` | Resume a display session or a port-owned app on its owning terminal. Without an ID, restore the calling port's last suspended app. |
| `session` | `session close <id>` | Close a display app, display shell, port-owned app, or port shell session. |
| `session` | `session send <id> <command> [args...]` | Run a command on an active display-shell session. The target must be at an empty prompt. |
| `session` | `session background` | Explain the foreground/background controls. |

Port shells default to `--term auto`. Auto mode sends a terminal Device
Attributes probe; a recognizable response enables VT100-style cursor controls
and a size probe, while no response falls back to a dumb line-oriented shell.
Use `--term vt100` or `--term ansi` to force escape-sequence output,
`--term dumb` for plain text, and `--size COLSxROWS` to set the terminal
dimensions without probing. Character encoding is independent of that profile:
port shells default to `--charset utf8`; use `--charset ascii` when a legacy
terminal displays Unicode TUI glyphs as unrelated code-page characters.

`jobs` prints a compact table that fits the built-in display terminal:

```text
NAME         STATE    STACK KIND        EVT  TICKS RES
batmon       running      - background  tick    17   1
log          stopped   6144 background  tick     0   0
```

Columns:

| Column | Meaning |
| --- | --- |
| `NAME` | Job registry name. |
| `STATE` | `stopped`, `waiting`, `running`, or `failed`. A waiting launch retries automatically. |
| `STACK` | Declared worker-stack admission requirement in bytes; `-` means no dedicated stack is declared. Dynamic allocations are not included. |
| `KIND` | Job kind. Current registry jobs are background workers. |
| `EVT` | `tick` if the job receives periodic tick events, otherwise `-`. |
| `TICKS` | Number of dispatched tick events while running. |
| `RES` | Number of resources currently recorded for the job. |

Use `job status <name>` for the job summary, owner string, last error, effective
worker-stack placement, tick interval/deadline, last and maximum handler time,
deadline-miss count, and resource details. Running rows are bold. A `waiting`
job has retained its launch request until the stack can be admitted while
preserving the internal-memory reserve; a `failed` job completed a start attempt
with an error. `sessions` prints the same timing telemetry for display and
port sessions as one row per session in `ID TITLE APP STATE TIME` order. In the
`TIME` column, the first pair is `interval/deadline` in milliseconds, the second
is `last/max` in microseconds, `n` is the dispatch count, and `!` is the deadline
miss count. Job timing detail uses the same values with explicit labels.
Job-owned resources use owner strings such as `job:log`; port
conflicts are reported as readable messages such as `job log owns cdc0`.

Common job examples:

```text
session create shell cdc0
session create shell uart0 --term ansi --charset ascii --size 80x25
session create shell lcd0
session create files display0
session focus display0
job start log cdc0
job start log file /.shell/log info
job start bridge cdc0 uart0
job start bridge uart0 link0 broadcast
link stream create link0 vser0 0x12345678
job start bridge cdc0 vser0
job start gpio-keys gpio17:UP gpio2:ENTER
job start gpio-keys --config /flash/gpio-keys.conf
job start httpd /www
job start displayd [display-target]   # display0 by default, web0 when headless
job start ntp-sync once
job start batmon 60
job start slip uart0 115200
job stop log
```

Only one instance of each built-in job name is active at a time. Starting the
same job again stops the previous instance and starts it with the new arguments.

## Filesystems And Storage

| Command | Usage | Description |
| --- | --- | --- |
| `disk` | `disk [status]` | Show persistent-storage status. |
| `disk` | `disk lsblk` | List internal flash and detected removable block devices and partitions. |
| `disk` | `disk mount [flash\|sd0pN] [mount]` | Mount the default volume or an explicit persistent volume. |
| `disk` | `disk umount [flash\|sd0pN\|mount]` | Unmount the default volume or an explicit volume/mount point. |
| `disk` | `disk format <flash\|sd0\|sd0pN> --force` | Create a FAT filesystem, permanently erasing the unmounted target. |
| `ramfs` | `ramfs [status]` | List PSRAM-backed volatile filesystem mounts. |
| `ramfs` | `ramfs mount /path size` | Mount a volatile filesystem that reserves PSRAM, such as `ramfs mount / 1m`. |
| `ramfs` | `ramfs unmount /path` | Unmount a ramfs mount. |
| `df` | `df` | Show free space on mounted storage volumes. |
| `cd` | `cd [path]` | Change current shell directory. |
| `ls` | `ls [-a] [-h] [path|pattern]` | List files. Hidden files are shown only with `-a`; sizes are human-readable with `-h`. |
| `cat` | `cat <path|pattern>` | Print a small text file. |
| `mkdir` | `mkdir <path> [path...]` | Create directories. |
| `rm` | `rm [-f|-rf] <path|pattern> [path|pattern...]` | Remove files. `-f` allows directories; `-rf` removes directories recursively. |
| `mv` | `mv <source|pattern> <dest>` | Rename or move a file or matched set. |
| `cp` | `cp <source|pattern> <dest>` | Copy a file or matched set. |
| `zip` | `zip [-0] <archive.zip> <path|pattern> [path|pattern...]` | Create a ZIP archive. `-0` stores without compression. |
| `unzip` | `unzip [-l] <archive.zip> [dest]` | List or extract a ZIP archive. |

Examples:

```text
ls -ah /.ssh
cp *.txt /backup
rm -rf /tmp/old
disk mount sd0p2 /mnt
ramfs mount /tmp 1m
ramfs mount / 4m
ramfs unmount /tmp
zip /books/archive.zip /books/*.txt
unzip -l /books/archive.zip
```

## Streams, Logs, Ports, And Transfers

| Command | Usage | Description |
| --- | --- | --- |
| `stream` | `stream` or `stream list` | List dynamic typed stream endpoints. |
| `stream` | `stream status <id>` | Show type, direction, provider, format, owner, and counters for one stream. |
| `daq` | `daq help` | Print DAQ usage. |
| `daq` | `daq status` | Show DAQ job status. |
| `daq` | `daq streams` | List stream IDs. |
| `daq` | See below | Start or stop data acquisition. |
| `log` | `log status` | Show runtime log ring status. |
| `log` | `log show [count]` | Print recent SolarOS log entries. |
| `log` | `log follow [error|warn|info|debug]` | Follow logs in the current shell. |
| `log` | `log clear` | Clear the runtime log ring. |
| `log` | `log level [error|warn|info|debug]` | Show or change runtime log level. |
| `log` | `log sink cdc [on|off]` | Enable or disable CDC mirroring of SolarOS logs. |
| `port` | `port list` | List byte-stream ports. |
| `port` | `port status <name>` | Show port capabilities and owner. |
| `xfer` | See below | Send or receive files over a byte-stream port. |

DAQ usage:

```text
daq start <file.csv> <stream...> [--rate seconds|--rate-ms ms]
daq start <stream...> <file.csv> [--rate seconds|--rate-ms ms]
daq start <file.csv> <stream> --changes [--append|--replace]
daq start <file.bin> <byte-or-audio-stream> --raw [--rate-ms ms]
daq stop
```

DAQ examples:

```text
daq start /logs/env.csv temperature humidity battery --rate 60
daq start /logs/key.csv gpio17 --changes
daq start /logs/uart0.bin uart0 --raw --rate-ms 25
daq start /logs/microphones.pcm audio0.capture --raw
```

`daq` CSV rows include `uptime_ms`, and include UTC `time_ms` when wall-clock time is
trusted. Raw mode accepts one byte or audio source and writes its data directly
without CSV framing. Audio uses the native PCM format reported by `stream
status`; `arecord` writes the same input as a WAV file. `recorder` adds
interactive stream and WAV-format selection, no-file live input monitoring,
hardware input gain when the selected device supports it, visualization,
pause, and playback.

Streams are runtime-registered endpoints, similar to services and displays.
Providers can add or remove scalar sensor, event, byte, and PCM audio streams.
Direction is `source`, `sink`, or `duplex`; sharing and current ownership are
reported by `stream status`. Board audio devices currently publish
`audio0.capture` and/or `audio0.playback`. Microphone level compatibility
streams remain available as `mic0` and `mic1` where the board has two input
channels.

Transfer usage:

```text
xfer protocols
xfer send <port> <file> --raw [-d ms]
xfer recv <port> <file> --raw [--append|--replace] [--idle-ms ms]
xfer send <port> <file> --zmodem
xfer recv <port> <file> --zmodem [--append|--replace]
```

`raw` and `zmodem` are supported. `kermit` is reserved but not implemented.

## Networking

| Command | Usage | Description |
| --- | --- | --- |
| `wifi` | `wifi` | Open the Wi-Fi display TUI when launched from the display shell. |
| `wifi` | `wifi status` | Show station/AP/NAT state and the current/next boot setting. |
| `wifi` | `wifi enable` | Save Wi-Fi enabled for the next boot. The current boot is unchanged. |
| `wifi` | `wifi disable` | Save Wi-Fi disabled for the next boot. The current boot is unchanged. |
| `wifi` | `wifi on` | Start Wi-Fi station mode and connect to remembered networks. |
| `wifi` | `wifi off` | Stop station/AP networking; an active ESP-NOW lease retains the radio. |
| `wifi` | `wifi scan` | Scan access points. |
| `wifi` | `wifi connect [ssid [password]]` | Connect and save/update a station profile. |
| `wifi` | `wifi disconnect` | Disconnect station mode. |
| `wifi` | `wifi known` | List remembered station profiles. |
| `wifi` | `wifi forget [ssid|all]` | Remove one or all remembered station profiles. |
| `wifi ap` | `wifi ap [status]` | Show SoftAP status. |
| `wifi ap` | `wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]` | Start and save SoftAP settings. |
| `wifi ap` | `wifi ap off` | Stop SoftAP. |
| `wifi nat` | `wifi nat [status|on|off]` | Configure IPv4 NAT for APSTA. |
| `wireguard` | `wireguard [status]` | Show profile, tunnel, route, peer, DNS, and kill-switch state without printing key material. |
| `wireguard` | `wireguard import <file>` | Validate one standard WireGuard client profile and save it in NVS. The source file is not removed. |
| `wireguard` | `wireguard forget` | Logically remove the saved profile from NVS. Bring the tunnel down first. |
| `wireguard` | `wireguard up [fail-open\|fail-closed]` | Request the tunnel and reconnect it after Wi-Fi address changes. The default is fail-closed for a full tunnel and fail-open for a split tunnel. |
| `wireguard` | `wireguard down` | Stop the tunnel, remove its routes, restore DNS, and disable its kill switch. |
| `ble` | `ble [status]` | Show BLE keyboard state and the current/next boot setting. |
| `ble` | `ble enable` | Save BLE enabled for the next boot. The current boot is unchanged. |
| `ble` | `ble disable` | Save BLE disabled for the next boot. The current boot is unchanged. |
| `ble` | `ble scan` | Scan nearby BLE devices. |
| `ble` | `ble pair` | Start keyboard pairing. |
| `ble` | `ble forget` | Erase the remembered keyboard from NVS and remove its BLE bond. |
| `ble gatt` | See below | Generic BLE GATT client. |
| `mqtt` | See below | MQTT/MQTTS client. |
| `ping` | `ping <host> [count]` | Send ICMP echo requests. Without count, ping runs until app-exit. |
| `netscan` | `netscan <host|range> [ports]` | Scan TCP ports on one host or a capped IPv4 range. |
| `ntp` | `ntp [server]` | Sync the wall clock from NTP. |

Wi-Fi is enabled by default when no saved setting exists, including after `nvs
clear`. `wifi on` and `wifi off` control the radio in the current boot. The
`wifi enable` and `wifi disable` settings take effect only after a reboot.
Disabling Wi-Fi does not erase saved station, access-point, or NAT settings.

WireGuard accepts one IPv4 interface address, one peer, one optional numeric
IPv4 DNS server, and at most eight IPv4 `AllowedIPs` prefixes. IPv6 addresses,
multiple peers, interface hooks, and configuration keys outside the documented
client subset are rejected. Use `wireguard down` before importing a replacement
profile or using `wireguard forget`. See [WireGuard VPN client](network.md#wireguard)
for routing, secret, and disconnect behavior.

BLE is enabled by default when no saved setting exists, including after `nvs
clear`. The `ble enable` and `ble disable` settings take effect only after a
reboot. Disabling BLE does not forget the remembered keyboard or erase its BLE
bond. On a BLE-disabled boot, SolarOS returns the unused Bluetooth controller
and host memory to the internal heap before normal service initialization.

BLE GATT usage:

```text
ble gatt status
ble gatt connect <aa:bb:cc:dd:ee:ff> <public|random|rpa_public|rpa_random>
ble gatt disconnect
ble gatt services
ble gatt chars <service-index>
ble gatt read <handle>
ble gatt write <handle> <hex...>
ble gatt write-nr <handle> <hex...>
```

MQTT usage:

```text
mqtt status
mqtt connect [url [username [password]]]
mqtt disconnect
mqtt publish <topic> <payload> [qos] [retain]
mqtt subscribe <topic> [qos]
```

`mqtt connect mqtt://host[:port] [username [password]]` and
`mqtt connect mqtts://host[:port] [username [password]]` save broker settings
in NVS. Later `mqtt connect` reuses the saved settings.

`netscan` accepts a host, same-subnet range, or compact range form. Examples:

```text
netscan 192.168.1.10 22,80,443
netscan 192.168.1.1-50 22
netscan wintermute 22
```

## OTA And Keys

| Command | Usage | Description |
| --- | --- | --- |
| `ota` | `ota status` | Show running and configured OTA state. |
| `ota` | `ota check` | Check signed board/flavor release metadata. |
| `ota` | `ota upgrade` | Download firmware into the inactive OTA partition and reboot into it. |
| `ota` | `ota url [url]` | Show or set the OTA base URL. |
| `ota` | `ota flavor [flavor]` | Show or set target OTA flavor. |
| `ota` | `ota boot 0|1` | Select an OTA slot and reboot. |
| `sshkey` | `sshkey [status]` | Show default SSH key status. |
| `sshkey` | `sshkey gen [-f] [2048|3072|4096]` | Generate `/.ssh/id_rsa` and `/.ssh/id_rsa.pub`. |
| `sshkey` | `sshkey pub` | Print the default public key. |
| `sshkey` | `sshkey rm` | Remove the default key pair. |

OTA resolves the artifact for the compiled board and target flavor from the
configured release index, verifies the signed index, verifies firmware SHA-256,
and writes the inactive ESP-IDF OTA partition. A signed `ota check` also caches
the flavors present for the compiled board in RAM. `ota flavor` completion uses
that verified list. Before a check, completion asks you to run `ota check`;
changing the OTA URL or rebooting clears the list. If the selected flavor is
not present in the checked release, `ota check` lists the flavors that are
available for the compiled board.

## Hardware And Time

| Command | Usage | Description |
| --- | --- | --- |
| `battery` | `battery [status]` | Show voltage, estimated charge, power source, config, and monitor trend. |
| `battery` | `battery config` | Show battery capacity and voltage thresholds. |
| `battery` | `battery capacity [mAh]` | Show or set capacity estimate. |
| `battery` | `battery min_voltage [V|mV]` | Show or set low-voltage threshold. |
| `battery` | `battery max_voltage [V|mV]` | Show or set full/external-power shortcut threshold. |
| `audio` | `audio status` | Show audio state, global speaker level, tone queue, and active synth telemetry. |
| `audio` | `audio devices` | List registered audio devices and their capture/playback streams. |
| `audio` | `audio device <id>` | Show one audio device and its native PCM format. |
| `audio` | `audio default [auto\|<id>]` | Show or select the preferred playback device. `auto` restores first-compatible-device selection. |
| `audio` | `audio tone [hz] [ms] [volume]` | Play a diagnostic tone. |
| `audio` | `audio tone-async [hz] [ms] [volume]` | Queue a tone and return its request ID immediately. |
| `audio` | `audio queue` | Show asynchronous tone queue state and counters. |
| `audio` | `audio cancel <request-id>` | Cancel a queued or playing asynchronous tone request. |
| `audio` | `audio level [volume]` | Show or set global speaker level. |
| `audio` | `audio mic [ms]` | Sample microphone level. |
| `audio` | `audio loopback [ms] [volume]` | Run microphone-to-speaker loopback. |
| `audio` | `audio off` | Stop audio output. |
| `led` | `led [status|on|off|toggle]` | Inspect or control the built-in status LED when available. |
| `expansion` | `expansion [status]` | Show expansion capabilities, named buses and leases, connector resources, active devices, and resource claims. |
| `expansion` | `expansion layout [connector]` | Draw the board's physical connector map with live free, releasable, claimed, fixed, power, ground, and NC markers. |
| `expansion` | `expansion scan` | List expansion resources and probe-capable drivers. |
| `expansion` | `expansion drivers` | List compiled expansion drivers. |
| `expansion` | `expansion devices` | List manually attached expansion devices. |
| `expansion` | `expansion bus create i2c <name> port=<i2c0\|i2c1> sda=<gpio> scl=<gpio> [speed=<hz>]` | Define a runtime I2C bus on an unused controller and approved expansion pins. |
| `expansion` | `expansion bus create onewire <name> pin=<gpio>` | Define a runtime named 1-Wire bus on an approved expansion pin. |
| `expansion` | `expansion bus create ps2 <name> clock=<gpio> data=<gpio>` | Define an exclusive PS/2 bus on two approved expansion pins. |
| `expansion` | `expansion bus create midi <name> tx=<gpio> rx=<gpio> [baud=<rate>]` | Define an exclusive MIDI bus; SolarOS selects the UART backend and defaults to 31250 baud. |
| `expansion` | `expansion bus create spi <name> host=<spi2\|spi3> sclk=<gpio> mosi=<gpio> [miso=<gpio\|none>] cs=<gpio> [cs=<gpio> ...] [max=<bytes>]` | Define a runtime-routed SPI bus on a board-approved host and expansion pins. |
| `expansion` | `expansion bus create uart <name> port=<uart1\|uart2> tx=<gpio> rx=<gpio> [baud=<rate>]` | Define a lazy runtime UART on an unused controller and approved expansion pins. |
| `expansion` | `expansion bus attach <name>` | Attach a named detachable bus and reserve its endpoint and signal pins. |
| `expansion` | `expansion bus detach <name>` | Detach an idle named bus, preserving its descriptor while releasing its endpoint and signal pins. |
| `expansion` | `expansion bus remove <name>` | Remove an idle runtime bus and release its signal pins. |
| `expansion` | `expansion attach <driver> <name> <resource...>` | Attach a compiled expansion driver or manual resource profile. |
| `expansion` | `expansion detach <name>` | Detach an active expansion device and release its resource claims. |
| `neopixel` | `neopixel [status\|list] [name]` | List attached WS2812/NeoPixel strips. |
| `neopixel` | `neopixel set <name> <index> <red> <green> <blue>` | Set one zero-based pixel and immediately refresh the strip. |
| `neopixel` | `neopixel fill <name> <red> <green> <blue>` | Fill and immediately refresh the strip. Color components are `0..255`. |
| `neopixel` | `neopixel clear\|show <name>` | Clear a strip immediately, or transmit its buffered colors. |
| `midi` | `midi status` | Show MIDI worker, traffic, parser, and queue status. |
| `midi` | `midi note-on\|note-off <channel> <note> [velocity]` | Queue a MIDI note message for transmission. |
| `midi` | `midi cc <channel> <controller> <value>` | Queue a MIDI control-change message. |
| `midi` | `midi program <channel> <program>` | Queue a MIDI program-change message. |
| `midi` | `midi send <status> [data1] [data2]` | Queue one validated raw MIDI message. |
| `control` | `control list\|parameters\|bindings` | Inspect normalized controls, native app parameters, or target bindings. |
| `control` | `control create <name> <stream> <min> <max> [smooth=ms] [deadband=value] [invert]` | Normalize a scalar stream as a named continuous control; use `manual` for script-supplied values. |
| `control` | `control bind <name> parameter <path> [pickup=on\|off]` | Bind a control to a typed native-app parameter with optional soft takeover. |
| `control` | `control bind <name> midi <channel> <cc>` | Bind a control to a MIDI Control Change target. |
| `control` | `control get\|set <name> [value]` | Read or set a normalized `0..65535` control value. |
| `control` | `control parameter get\|set <path> [value]` | Read or set an available native parameter in its declared unit. |
| `control` | `control unbind <name>` | Remove all target bindings owned by one named control. |
| `control` | `control delete <name>` or `control clear` | Remove one control and its bindings, or remove all controls and bindings. |
| `radio` | `radio` | Open the packet-radio TUI with live status and editable common config. |
| `radio` | `radio status|list` | List packet radios registered by expansion drivers. |
| `radio` | `radio status <name>` | Show one packet radio, its capabilities, state, and current config. |
| `radio` | `radio config <name> [field value]` | Show or update common packet-radio configuration. |
| `radio` | `radio profile list` | List immutable built-in and persistent user radio profiles. |
| `radio` | `radio profile show <profile>` | Show every setting captured by one profile. |
| `radio` | `radio profile apply <radio> <profile>` | Apply one complete profile to a radio, restoring the prior config if application fails. |
| `radio` | `radio profile save <radio> <profile>` | Save or replace a user profile from the radio's complete current config. |
| `radio` | `radio profile remove <profile>` | Remove a user profile. Built-in profiles are read-only. |
| `meshcore` | `meshcore status` | Show MeshCore identity, radio, packet, delivery, duplicate, memory, and stack state. |
| `meshcore` | `meshcore identity show\|generate\|import\|export` | Inspect or explicitly manage the private MeshCore identity. |
| `meshcore` | `meshcore name [name]` | Show or set the MeshCore-specific advertised name. |
| `meshcore` | `meshcore advert zero\|flood` | Queue a local or explicitly network-wide advert. |
| `meshcore` | `meshcore channel list\|add\|remove\|public` | Manage bounded shared-key groups while the job is stopped. |
| `radio` | `radio state <name> [sleep|standby|rx|tx]` | Show or change radio operating state. |
| `radio` | `radio send <name> <text|byte...>` | Send one packet. |
| `radio` | `radio recv <name> [timeout-ms]` | Receive one packet and print metadata plus payload. |
| `espnow` | `espnow [status]` | Show ESP-NOW owner, channel, PHY, peers, traffic, drops, conflicts, and last error. |
| `espnow` | `espnow peers\|list` | List persistent configured and volatile learned Link-ID-to-MAC mappings. |
| `espnow` | `espnow peer add <link-id> <mac>` | Save a persistent unicast peer mapping. |
| `espnow` | `espnow peer remove <link-id>` | Remove a configured or learned peer mapping. |
| `link` | `link status\|list` | List active SolarOS Link instances and their queue/protocol counters. |
| `link` | `link status <link>` | Show local ID, transport MTU, queues, acknowledgements, duplicates, CRC errors, and drops. |
| `link` | `link send <link> <broadcast\|destination-id> <text>` | Queue a text message. Unicast requests an acknowledgement. |
| `link` | `link send-binary <link> <broadcast\|destination-id> <byte...>` | Queue a binary message. |
| `link` | `link receive <link> [timeout-ms]` | Remove and print one received message. |
| `link` | `link stream list` | List Link-backed virtual serial ports. |
| `link` | `link stream status [port]` | Show virtual-port peer, connection, queue, traffic, retry, reconnect, and error state. |
| `link` | `link stream create <link> <port> <peer-id>` | Register a reliable peer-bound Link stream as a normal SolarOS byte-stream port. |
| `link` | `link stream remove <port>` | Remove an unclaimed Link stream port. |
| `pocsag` | `pocsag status` | Show detailed status for the POCSAG background receiver. |
| `pocsag` | `pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha\|numeric] [normal\|inverted] [function]` | Encode and transmit one POCSAG page. |
| `uart` | `uart [status [bus]]` | Show the default `uart0` or a selected named UART bus. |
| `uart` | `uart baud [bus] [rate]` | Show or set a named UART bus baud rate. |
| `uart` | `uart mode [bus] [raw\|line]` | Show or set a named UART bus service mode. |
| `uart` | `uart write [bus] <text>` | Write text through the default or selected named UART bus. |
| `uart` | `uart read [bus] [ms]` | Read bytes from the default or selected named UART bus. |
| `gpio` | `gpio status` or `gpio list` | List board GPIOs with free, releasable, or fixed pin policy. |
| `gpio` | `gpio mode <pin> <in|out> [none|up|down]` | Configure a runtime GPIO. |
| `gpio` | `gpio read <pin>` | Read a runtime GPIO. |
| `gpio` | `gpio write <pin> <0|1>` | Write a runtime GPIO configured as output. |
| `gpio` | `gpio release <pin>` | Reset a direct GPIO and release its resource claim for a bus or another service. |
| `onewire` | `onewire [status [bus]]` | Show every registered named 1-Wire bus, or one selected bus. |
| `onewire` | `onewire reset <bus\|pin>` | Reset a named bus or direct runtime GPIO and report presence. |
| `onewire` | `onewire scan <bus\|pin>` | Discover and list 1-Wire ROM addresses. |
| `onewire` | `onewire xfer <bus\|pin> <read-len> [byte...]` | Reset, write bytes, then read bytes on a 1-Wire target. |
| `adc` | `adc status` | Show ADC service status. |
| `adc` | `adc read <pin>` | Read an ADC-capable runtime pin. |
| `dpad` | `dpad [status]` | Show ADC D-pad pins, raw values, zones, and calibration thresholds. |
| `dpad` | `dpad calibrate [idle]` | Calibrate the current D-pad idle value. |
| `dpad` | `dpad calibrate reset` | Restore the compiled D-pad calibration. |
| `joystick` | `joystick [status]` | Show joystick axes, raw values, direction, and thresholds. |
| `joystick` | `joystick calibrate` | Calibrate the current joystick center. |
| `joystick` | `joystick calibrate reset` | Restore the compiled joystick calibration. |
| `pwm` | `pwm status` | Show PWM state. |
| `pwm` | `pwm set <pin> <freq-hz> <duty-percent>` | Start LEDC PWM on a runtime pin. |
| `pwm` | `pwm off <pin>` | Stop PWM on a pin. |
| `i2c` | `i2c [status [bus]]` | Show every named I2C bus, or one selected bus. |
| `i2c` | `i2c speed [bus] [hz]` | Show or change a named bus clock; defaults to `i2c0` and accepts 1 through 1000000 Hz. |
| `i2c` | `i2c scan [bus]` | Scan a named bus; defaults to `i2c0`. |
| `i2c` | `i2c probe [bus] <addr>` | Probe one address; defaults to `i2c0`. |
| `i2c` | `i2c read [bus] <addr> <reg> [len]` | Read register bytes; defaults to `i2c0`. |
| `i2c` | `i2c write [bus] <addr> <reg> <byte...>` | Write register bytes; defaults to `i2c0`. |
| `spi` | `spi [status [bus]]` | Show every named SPI bus, or one selected bus. |
| `spi` | `spi xfer <bus> <cs> <mode> <hz> <byte...>` | Full-duplex transfer over a named SPI bus. |
| `spi` | `spi read <bus> <cs> <mode> <hz> <len> [fill]` | Read bytes over a named SPI bus. |
| `spi` | `spi write <bus> <cs> <mode> <hz> <byte...>` | Write bytes over a named SPI bus. |
| `date` | `date [YYYY-MM-DD]` | Show or set the local date. |
| `time` | `time [HH:MM[:SS]]` | Show or set the local time. |
| `temperature` | `temperature` | Read the board temperature sensor when available. |
| `humidity` | `humidity` | Read the board humidity sensor when available. |

Board-specific connector resources, runtime GPIO policy, named buses, leases,
and attachment examples are documented in [Expansion Ports](expansion.md).
Use `expansion status` and `gpio list` for the authoritative view on a running
device.

The `onewire` command accepts a registered bus name or any runtime-accessible
GPIO. `onewire status` discovers named buses, while the numeric form preserves
the direct-pin workflow. Every `xfer` starts with a 1-Wire reset, writes the
supplied bytes, and then reads `read-len` bytes. For example,
`onewire xfer 1 9 0xcc 0xbe` issues Skip ROM and Read Scratchpad, then reads a
nine-byte scratchpad. The equivalent named form starts with
`onewire xfer onewire0`. ROM address bytes supplied to `xfer` use
least-significant-byte-first wire order. The service enables the ESP32 internal
pull-up, but a 4.7 kohm external pull-up from the data line to 3.3 V is strongly
recommended. The internal pull-up is not a parasite-power supply.

Physical displays are listed by `display list`. A built-in board panel registers
as a board display target such as `display0`; an expansion display driver stays
in `expansion drivers` as attachable hardware and registers a display target
after it is attached. The built-in board panel is not an expansion driver.
`display list` includes the current owner when a target is claimed. `display
test <target>` claims the target while it draws a visible frame/test pattern,
then releases it. `display mode <target>` lists driver-specific display
settings for supported display drivers; `display mode <target> <mode>` applies
one setting. With `power=auto`, the built-in ST7305 path uses the normal power
profile before writing changed frame content and switches to the paired `lpm`
profile after the frame has been idle for the configured driver debounce, or
immediately when a present pass finds no changed pixels. The default ST7305
idle debounce is 1000 ms. The advanced
`display mode <target> idle-lpm-ms=<ms>` driver option updates it at runtime
and persists it in the ST7305 NVS namespace. ST7305 tuning options also live on
this driver-specific mode surface: `power=<auto|hpm|lpm>` selects automatic
idle switching or a forced power mode, `inverted=<on|off>` selects panel
inversion, `lpm-hz=<0.25|0.5|1|2|4|8>` changes the controller's LPM frame-rate
field, and `hpm-hz=<16|25.5|32|51>` changes the controller's HPM frame-rate
field. These driver values are stored in NVS when changed. The ST7305
`inverted=` setting controls panel polarity and remains independent of the
terminal palette selected with `setterm palette`.
On SSD1683 board and expansion targets, `refresh=auto` uses a full waveform for
the first changed frame and after every 19 non-full updates, while unchanged
frames are skipped. Waveshare V2 expansion targets use the changed framebuffer
rectangle and the controller's partial-window waveform for those intermediate
updates. `refresh=fast` forces a fast full-frame waveform and `refresh=full`
forces the full cleanup waveform on every changed frame.

Packet radio devices are datagram endpoints registered by expansion drivers, not
byte-stream ports. The common radio layer preserves packet metadata such as RSSI
and optional source/destination IDs. Radio frequency values are Hz by default
and also accept `k`, `kHz`, `M`, and `MHz` suffixes:

```text
radio status radio0
radio config radio0 frequency 433MHz
radio config radio0 modulation gfsk
radio send radio0 hello
radio recv radio0 5000
```

Profiles avoid partially reconfiguring one end while copying a list of fields.
`lora-eu868`, `gfsk-eu868`, and `ook-eu868` are built in. Up to eight user
profiles are stored as one versioned NVS record and consume no idle profile
cache:

```text
radio profile list
radio profile apply radio0 lora-eu868
radio config radio0 sf 9
radio profile save radio0 lora-sf9
radio profile show lora-sf9
radio profile remove lora-sf9
```

Like `radio config`, applying a profile leaves the radio in driver standby. If
the driver rejects a setting, SolarOS attempts to restore the complete prior
configuration and operating state. A profile does not bypass the driver's
supported frequency, modulation, power, or packet-size checks. The operator
remains responsible for regional frequency, transmit-power, and duty-cycle
rules.

RFM95W radios support FSK, GFSK, MSK, GMSK, OOK, and LoRa. LoRa additionally
uses `bandwidth`, `sf`, and `coding-rate`. For an RFM95W using the common
868 MHz profile:

```text
radio config radio0 frequency 868MHz
radio config radio0 bandwidth 125000
radio config radio0 sf 7
radio config radio0 coding-rate 4/5
```

`variable=on` selects the normal explicit LoRa header. `variable=off` selects
implicit-header mode and requires the configured `length` to match on both
ends. SF6 requires implicit-header mode.

FSK-family and OOK modes use `bitrate`, `deviation`, and a single-side
`bandwidth`. MSK and GMSK derive deviation as one quarter of bitrate; GFSK and
GMSK use Gaussian BT=1.0 shaping. RFM95W FSK/OOK packets contain at most 64
payload bytes, while fixed length zero enables the unlimited FIFO-stream mode.

The POCSAG job configures an attached packet radio for one paging channel,
filters addresses to one RIC, corrects up to two bad bits per BCH codeword, and
publishes decoded pages to the universal inbox. Consecutive POCSAG batches are
kept in one message. For the 448.425 MHz test channel:

```text
job start pocsag radio 448425000 1200 1841525 alpha
pocsag status
inbox list unread
```

Use `inverted` as the final argument if the transmitter and receiver use
opposite FSK mark/space polarity. Stopping the job restores the radio's previous
configuration and state.

To transmit a page, stop the receiver when it uses the same half-duplex radio,
then send the message. Alphanumeric pages default to function 3; numeric pages
default to function 0. The optional final argument selects function 0 through 3.
The previous radio configuration and state are restored after transmission.

```text
job stop pocsag
pocsag send radio 448425000 1200 1841525 "SolarOS calling" alpha inverted
```

## Quick Examples

```text
help
version
pkg
board
wifi on
ping wintermute
sshkey gen 2048
ota check
ota upgrade
watch -n 1 battery
daq start /logs/env.csv temperature humidity battery --rate 60
session create shell cdc0 --term auto
session create shell lcd0
xfer send uart0 /logs/payload.bin --zmodem
```

## Quick reference

Run `commands` to list commands compiled into the current firmware, `man TOPIC`
for a focused guide, and `help` for the complete manual tree. Commands are
package-aware, support shell completion where applicable, and use the current
shell working directory for relative paths.
