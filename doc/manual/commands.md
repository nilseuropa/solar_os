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

Tables fit the current shell width on display and port shells. Long rows are
clipped at the right edge; trailing values or columns can be omitted. Widen the
terminal to see more columns. One cell is reserved to prevent automatic wrapping.

Paths are resolved relative to the current shell directory. The default storage
volume is presented as `/`. On SD-backed boards, the primary SD card volume also
has the internal mount path `/sdcard`, and the internal flash volume is mounted
at `/flash`. On boards without SD support, internal flash is mounted as `/`.

When the path displayed in the shell prompt exceeds half the terminal width,
the prompt shows only the last directory, such as `/logs/`. The working
directory and relative path resolution remain unchanged.

Wildcard patterns are supported by selected filesystem commands, for example
`*.txt`.

Tab completion covers commands, subcommands, filesystem paths, job names, port
names, and stream IDs where the command exposes enough structure.
Completed paths that contain spaces are inserted as quoted shell tokens.
For `ssh` and `scp`, it also reads host aliases from `/.ssh/hosts`. An explicit
`user@` prefix is preserved; a unique SCP host match appends `:` for the remote
path.

On display shells and VT100 port shells, `Backspace` removes the character
before the command-line cursor. `Delete` removes the character under the cursor
without moving it; it does nothing at the end of the line.

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
bit for this shorthand. Use `echo` for script progress messages and `wait` to
insert a whole-second delay between commands. `wait` does not put the device
into light sleep. A foreground application
launch ends the current script; the script does not resume after the application
closes.

History is kept in memory and cached at `/.shell/history` when storage is
available. The optional user alias file follows the default storage volume:

```text
/.shell/alias
```

Playground separately maintains `/.shell/playground`. Do not edit that file;
installing, updating, or uninstalling community applications regenerates it.

The startup script source is selected with `setterm startup [auto|flash|sd]`
and is stored in NVS. `auto` is the default, including after `nvs clear`: it
uses a mounted board-owned SD card when available and otherwise falls back to
internal flash. `flash` and `sd` are explicit selections without fallback. On
an SD-capable board, the paths are `/flash/.shell/startup` and
`/sdcard/.shell/startup`; on a board without SD, internal flash is mounted at
`/`, so its path is `/.shell/startup`. The script runs once per boot on the
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
| `help` | `help [TOPIC]`; `help command.status`; `help status`; `help update`; `help reset` | Browse the package-aware manual or manage its signed exact-version SD copy. `command.status` escapes the maintenance keyword. |
| `man` | `man TOPIC`; `man -k QUERY...`; `man --list` | Read or search the package-aware SolarOS manual. |
| `clear` | `clear` | Clear the active shell terminal. |
| `echo` | `echo [text...]` | Print the arguments separated by spaces, followed by a newline. Quotes preserve spaces and are not printed. |
| `wait` | `wait <seconds>` | Pause the calling shell or shell script for 0 through 86400 seconds. |
| `watch` | `watch [-n seconds] <command> [args...]` | Repeat another shell command until `Esc`, `q`, or the app-exit key is pressed. |
| `sh` | `sh <file>` | Run a simple SolarOS shell script from storage. |
| `exit` | `exit` | Close the current UART, USB CDC, or telnet shell when another interactive shell remains. |
| `reboot` | `reboot` | Restart the board. |
| `nvs` | `nvs status` | Show the default NVS partition size, entry usage, and namespace count. |
| `nvs` | `nvs list [namespace]` | List non-empty namespaces and their entry usage, or list one namespace's keys, types, sizes, and storage cost. Values are never displayed. |
| `nvs` | `nvs erase <namespace> [key]` | Erase one key or all data in one namespace, then reboot. |
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
| `messages` | `messages` | Open the tabbed instance, connection, and message manager. |
| `messages` | `messages status` | Show archive, persistence, drop, and live provider state. |
| `messages` | `messages conversations` | List provider-neutral conversations and unread/security state. |
| `messages` | `messages list <conversation-id>` | List retained messages and their stable hexadecimal IDs. |
| `messages` | `messages send <conversation-id> <text> [--allow-untrusted]` | Queue an outbound message; discovered direct endpoints require the explicit flag. |
| `messages` | `messages read <conversation-id>` | Mark a conversation and its linked Inbox entries read. |
| `messages` | `messages delete <message-id>` | Delete one retained message and its linked Inbox projection by hexadecimal ID. |
| `messages` | `messages clear <instance\|instance-id\|all>` | Clear retained history and owned Inbox projections, including for removed instances. Unrelated Inbox sources remain. |
| `messages` | `messages remove <instance\|instance-id>` | Remove a provider instance, connection credentials, pending sends, history and Inbox notifications. |
| `messages` | `messages outbox` | List pending outbound requests in queue order. |
| `messages` | `messages cancel <message-id>` | Cancel a queued outbound message by the hexadecimal ID printed by `list` or `send`. |
| `outbox` | `outbox [list]` | List pending outbound messages. Sent and failed messages remain in conversation history, not Outbox. |
| `outbox` | `outbox cancel <message-id>` | Cancel one pending message by hexadecimal ID. |
| `gateway` | `gateway status` | Show gateway configuration, connection state, and traffic counters. |
| `irc` | `irc add <instance> <host> <port> <nick>`; `irc set <instance> <host> <port> <nick>` | Save an IRC server profile on mounted storage. |
| `irc` | `irc status`; `irc connect\|disconnect <instance>` | Inspect or enable individual IRC connections maintained by `irc-sync`. |
| `irc` | `irc remove <instance>` | Remove an IRC instance, its credentials, history and associated Inbox notifications. |
| `irc` | `irc join\|query <instance> <channel\|nick>`; `irc leave <conversation-id>` | Open rooms and private conversations through shared Messaging. |
| `irc` | `irc sasl <instance> <account> <password>`; `irc sasl <instance> off`; `irc pass <instance> <password\|off>`; `irc ca <instance> <PEM-path\|default>` | Set optional authentication and TLS trust. See `man messaging.irc`. |
| `sc2` | `sc2 id`; `sc2 status` | Show this device's SC2 end-to-end identity (device ID, fingerprint, prekeys) and the service state. Creates the identity on first use. See `man python.sc2`. |
| `sc2` | `sc2 safety <peer>` | Show the safety number shared with a stored peer, to compare out of band. `<peer>` is a 32-hex device ID, a unique prefix (6+ hex digits) of a known peer, or an alias. |
| `sc2` | `sc2 verify <peer>`; `sc2 unverify <peer>` | Mark the safety number with a peer as compared (or take that back). Chat shows sc2 DMs as `(verified)` or `(unverified)`; the record is kept in the SC2 store (up to 64 peers). Verifying needs the peer's keys (message it first). |
| `sc2` | `sc2 alias`; `sc2 alias <name> <peer>`; `sc2 alias <name> off` | List, save or delete peer nicknames (up to 16, in the SC2 store) for `sc2 safety` and `messages query <sc2-instance> <peer>`. |
| `sc2` | `sc2 selftest`; `sc2 bench [n]` | Run the SC2 known-answer and two-device session tests in RAM, or time the primitives and a DM/room round trip. Neither touches stored keys. |
| `sc2` | `sc2 room create ["name"]` / `sc2 room invite <room-id\|name>` / `sc2 room join <sc2i:...\|@file>` / `sc2 rooms` | Create a private SC2 room, print its invite again, join with an invite, or list names, IDs and state. Quote names containing spaces (up to 48 UTF-8 bytes). Admission requires a valid end-to-end invite proof. Then `messages send <conv> <text>`; `messages leave <conv>` leaves. SC2 commands, peers and rooms support Tab completion. See `man messaging`. |
| `gateway` | `gateway configure <url> [token]` | Save gateway connection settings. The gateway uses the global SolarOS user identity. |
| `messages` | `messages classes` / `messages instances` | List available provider classes or configured instances. |
| `messages` | `messages add gateway <name> <url> [token]` | Save an independent Gateway instance. Enable it with `messages connect <name>` and start `gateway-sync`. |
| `messages` | `messages configure <name> <url> [token]` | Change a Gateway instance's connection settings. |
| `messages` | `messages add irc <name> <host> <port> <nick>` | Save an independent native IRC connection. |
| `messages` | `messages add meshcore <name> radio <radio> <profile>` | Save a MeshCore instance on a local radio. |
| `messages` | `messages add meshcore <name> ble <address> <type> [pin]` | Save a MeshCore companion binding. |
| `messages` | `messages add link <name> <transport>` | Bind an independent Chat projection to a named Link transport. |
| `messages` | `messages add sc2 <name> <domain> [admit] [key=<b64>]` | Add an SC2 relay from its signed descriptor at `https://<domain>/.well-known/solaros-relay` (CA and host name checked; never plain HTTP). The relay key (and the fallback relays and their keys) are pinned; every login must prove the key, a different key is refused. `key=` must match the descriptor when given. `messages configure <name> key <b64>\|off` changes the pin. See `man messaging`. |
| `messages` | `messages add sc2 <name> chats://<host>:<port>[#pin=sha256/<b64>] [admit]` | Save a disabled SC2 end-to-end relay instance. Enable it with `messages connect <name>` and start its worker with `job start sc2-sync`; `messages disconnect <name>` disables it. `messages configure <name> admit <code>\|off` or `pin <pin>\|off` change it; `fallback <url> [<url> <url>]\|off` sets up to three fallback relays (failover and return to the primary; `sc2 relay` shows which is in use). With a pin, the pinned key alone authenticates the relay (self-signed certificates work); without one the CA bundle is used. See `man messaging`. |
| `gateway` | `gateway connect [url] [token]` | Enable gateway synchronization, optionally updating settings. |
| `gateway` | `gateway disconnect` | Disable gateway synchronization. |
| `gateway` | `gateway rooms` | List known and joined gateway rooms. |
| `gateway` | `gateway join\|leave\|delete <room>` | Queue a gateway-specific room operation. |
| `email` | `email` | Open the receive-only email app. |
| `email` | `email status` | Show saved account, local message counts, and last sync error. |
| `email` | `email configure <imaps://host[:port]> <user> <password> [mailbox]` | Save an IMAPS account; the default mailbox is `INBOX`. |
| `email` | `email sync` | Start a one-shot mailbox synchronization. |
| `email` | `email forget` | Remove the saved account and local email list. |
| `rtsp-auth` | `rtsp-auth` | List stored RTSP client accounts (host, port, username; never the password). |
| `rtsp-auth` | `rtsp-auth set <host[:port]> <user> <password>` | Save a Basic/Digest account used when an `rtsp` URL has no inline userinfo. Four accounts. |
| `rtsp-auth` | `rtsp-auth clear [host[:port]]` | Remove one stored account, or every account when the host is omitted. |
| `pocsag` | `pocsag status` | Show POCSAG receiver configuration, counters, correction statistics, and RSSI. |
| `pocsag` | `pocsag send <radio> <frequency-hz> <baud> <ric> <message> [alpha\|numeric] [normal\|inverted] [function]` | Encode and transmit one POCSAG page. |

`nvs status` distinguishes raw free entries from entries currently available
for new data; use the available count when diagnosing a failed NVS write.
`nvs list` does not display values, so credentials and other secrets are not
printed. Namespace entry totals include the namespace record itself. Clearing a
namespace removes all of its keys, but ESP-IDF retains its one-entry namespace
record. `nvs erase` reboots after a successful change because running services
can cache NVS-backed settings. Use `nvs backup` before erasing unfamiliar
namespaces or keys.
`nvs backup` writes a versioned, CRC-protected image of the complete default NVS
partition. The file contains unencrypted credentials and settings, so protect
it like a password. `nvs restore` accepts only a complete backup for the current
NVS partition address and size, verifies its CRC before changing flash, verifies
the written partition again, and reboots. `nvs clear` erases the complete
default NVS partition, including identity, Wi-Fi and BLE state and remaining
service settings, then reboots. Migrated Chat credentials, radio profiles, and
ESP-NOW peers are files on mounted storage. Files on SD or the internal
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
SHA-256 before activation. Firmware carries a setup and recovery guide and a
searchable topic directory; full guides and scripting API references require
the downloaded manual. `help reset` returns immediately to the setup and
recovery guide.

User aliases are stored in `/.shell/alias`, one per line:

```text
name command-or-app fixed-args...
```

Arguments typed after the alias are appended.
SolarOS reads the user file before the managed `/.shell/playground` aliases, so
a user alias with the same name takes precedence. Native commands and firmware
applications always take precedence over both alias files.
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
| `pkg` | `pkg` | Open the native-module package manager. |
| `pkg` | `pkg system` | Print compiled package groups and build units. |
| `pkg` | `pkg available` | Verify the signed native-module catalog and print its compact module list. |
| `pkg` | `pkg installed` | List installed application, job, driver, and library modules. |
| `pkg` | `pkg install <module>` | Download, verify, validate, and atomically install a native module; Ctrl+C, Esc, or the app-exit key cancels. |
| `pkg` | `pkg remove <module>` | Remove an installed native module. |
| `pkg` | `pkg unload <library>` | Unload an unused library while keeping its installed file. |
| `load` | `load <file.elf> [args...]` | Validate, relocate, run, and unload one native ELF module from storage. |
| `board` | `board` | Print board ID, name, and capabilities. |
| `identity` | `identity [status]` | Show the configured user and hostname. |
| `identity` | `identity user <name>` | Save the SolarOS user and default SSH/SCP username in NVS. |
| `identity` | `identity hostname <name>` | Save the device hostname in NVS; reboot to update Wi-Fi. |
| `engine` | `engine [status|reset]` | Print or reset generic engine utilization counters for CPU/SIMD-style backends and vector bulk operations. |
| `display` | `display [list]`; `display layouts`; `display test <target>`; `display mode <target> [mode]` | List targets and layouts, draw a test pattern, or change driver-specific display settings. |
| `display` | `display relax <target> [seconds]` | Run timed native black/white recovery cycles on supported EPDs, then restore the saved image and refresh mode. |
| `dlp` | `dlp serve <display> stream=<byte-stream>`; `dlp status`; `dlp stop <display>` | Receive DLP images on a named display through an exclusive duplex byte stream. See [DLP](apps.md#dlp). |
| `display` | `display join <name> --horizontal\|--vertical <target> <target> [target ...]`; `display unjoin <name>` | Join two to four physical targets into one logical display, or remove the join. |
| `display` | `display split <target> --horizontal\|--vertical <first> <second>`; `display unsplit <target>` | Split one physical target into two equal logical viewports, or remove the split. |
| `input` | `input [status|keyboard|touch|mouse|joystick|dpad|buttons|gesture]` | List all input sources or filter them by semantic class. |
| `input` | `input test <source>` | Show event counters and the last key, pointer, axis, or gesture event accepted from one source. |
| `input` | `input calibrate <source> [set <min-x> <max-x> <min-y> <max-y> <width> <height>\|reset]` | Show, save, or reset coordinate calibration for an absolute-pointer source. |
| `input` | `input emit <key\|chord>` | Emit a local key tap, such as `RIGHT`, `ALT+RIGHT`, `ENTER`, or one literal character, through the normal input-focus path. |
| `input` | `input keymap [<source> [show\|load <path>\|reset]]` | List source mapping support, inspect a map, load a validated JSON file, or restore a registered source profile. See [source keymaps](input.keymap.md). |
| `gesture` | `gesture [status]` | List gesture-capable sensors and touchscreens, readiness, and the gesture kinds each source advertises. |
| `gesture` | `gesture bind source=<name\|*> gesture=<name> [direction=<name\|*>] [cooldown=<ms>] -- <command> [args...]` | Configure a volatile gesture-to-command rule for the `gestures` job. |
| `gesture` | `gesture bindings` | Show listener state and list volatile gesture bindings, trigger counts, queue drops, cooldowns, and commands. |
| `gesture` | `gesture unbind <id\|all>` | Remove one volatile gesture binding or all of them. |
| `status` | `status` | Print a compact system summary, including the last foreground-app exit code. |
| `uptime` | `uptime` | Print elapsed time since boot. |
| `mem` | `mem [policy]` | Print heap and managed PSRAM status; `policy` also shows allocation-class counters, guarded fallback limits, and the last tagged failure. |
| `mem` | `mem reclaim` | Unload unused resident libraries and show recovered PSRAM and the largest free block before and after. Libraries in use and application sessions remain resident. |
| `top` | `top` | Print a one-shot cumulative FreeRTOS task snapshot. Use `ltop` for live interval CPU, memory, and stack monitoring. |
| `sleep` | `sleep` | Enter explicit light sleep. |
| `deepsleep` | `deepsleep` | Turn off the ESP32 radios and enter deep sleep. KEY, an armed RTC or scheduled timer, or RESET starts a fresh boot. |
| `shutdown` | `shutdown` | Stop apps and jobs, flush storage, and power down. Only the physical power/KEY button, RESET, or a power cycle starts a fresh boot. |
| `suspend` | `suspend` | Turn off the primary display and temporarily use the `lowpower` profile while services and jobs continue. Press KEY or a keyboard key to resume. |
| `power` | `power [status]` | Show the selected and effective profiles, suspend state, sleep policy, and wake statistics. |
| `power` | `power profile [performance\|balanced\|battery\|lowpower]` | Show or save the power profile. |
| `power` | `power idle [off\|seconds]` | Show or configure the display-shell idle light-sleep timeout. |
| `power` | `power key [short\|long] [action]` | Show or configure the dedicated KEY short- and long-press actions. |
| `power` | `power sleep` | Enter explicit light sleep from the display shell. Press KEY to wake. |
| `power` | `power suspend` | Turn off the primary display while services and jobs continue. Press KEY or a keyboard key to resume. |
| `rtc` | `rtc [status]` | Show the RTC provider, capabilities, interrupt wiring, and alarm/timer owners. Reports `unavailable` when no RTC is present. |
| `rtc` | `rtc alarm set HH:MM[:SS] [day=N] [weekday=N]` | Program the RTC hardware alarm, optionally matching a day or weekday. |
| `rtc` | `rtc alarm clear` | Clear the RTC hardware alarm owned by this command. |
| `rtc` | `rtc timer set <duration> [repeat]` | Program the RTC countdown timer. Durations use an `s`, `m`, `h`, or `d` suffix. |
| `rtc` | `rtc timer clear` | Clear the RTC countdown timer owned by this command. |
| `rtc` | `rtc pending` | Show pending RTC alarm and timer interrupts. |
| `rtc` | `rtc ack <alarm\|timer\|all>` | Acknowledge pending RTC interrupts. |
| `schedule` | `schedule`; `schedule list` | List persistent alarms and scheduled shell scripts. Entries show their enabled state, trigger, and action. |
| `schedule` | `schedule show <name>` | Show one entry, including its run and skip counters. |
| `schedule` | `schedule add <name> in <duration> <alarm\|run script>` | Add a one-shot monotonic schedule. Durations use an `s`, `m`, `h`, or `d` suffix. |
| `schedule` | `schedule add <name> every <duration> <alarm\|run script>` | Add a recurring monotonic interval schedule. |
| `schedule` | `schedule add <name> at YYYY-MM-DD HH:MM[:SS] <alarm\|run script>` | Add a one-shot schedule in configured local time. It waits for valid wall-clock time. |
| `schedule` | `schedule add <name> daily HH:MM[:SS] <alarm\|run script>` | Add a daily schedule in configured local time. |
| `schedule` | `schedule add <name> weekly <sun,mon,...> HH:MM[:SS] <alarm\|run script>` | Add a schedule for the selected local weekdays. |
| `schedule` | `schedule enable <name>`; `schedule disable <name>` | Enable or disable an entry without removing it. |
| `schedule` | `schedule remove <name>` | Remove an entry. |
| `schedule` | `schedule run <name>` | Run an entry immediately. Only one scheduled shell script can run at a time. |
| `schedule` | `schedule stop [name]` | Stop the active ringing alarm, optionally only when its name matches. |
| `setterm` | `setterm` | Open the terminal settings TUI from the display shell. |
| `setterm` | `setterm --display <target> [orientation\|font\|textsize\|palette\|statusbar] [value]` | Show or change the volatile terminal profile of a named display target. |
| `setterm` | `setterm orientation [0\|90\|180\|270]` | Show or set primary-display orientation. |
| `setterm` | `setterm font [mono\|compact]`; `setterm textsize [8\|10\|12\|14\|16\|18\|20]` | Show or set the terminal font and text size. |
| `setterm` | `setterm palette [normal\|inverted]` | Show or set the logical terminal and shared-graphics palette. |
| `setterm` | `setterm foreground [#RRGGBB]`; `setterm background [#RRGGBB]` | Show or set the persistent RGB terminal theme colors. |
| `setterm` | `setterm statusbar [show\|hide]` | Show or hide the graphical shell status bar. |
| `setterm` | `setterm brightness [0..100]` | Show or set display brightness. |
| `setterm` | `setterm backlight [0..100\|follow]` | Show or set auxiliary lighting, such as the keyboard backlight, independently or following display brightness. |
| `setterm` | `setterm profile [vt100\|ansi\|dumb]`; `setterm charset [utf8\|ascii]` | Configure escape sequences and TUI glyph output for the current port shell. |
| `setterm` | `setterm keyboard [us\|de]`; `setterm powerkey [short\|long] [action]` | Show or set the keyboard layout and dedicated KEY action. |
| `setterm` | `setterm keyrate [off\|1..60 [delay-ms]]` | Show or set the shared keyboard and button repeat policy. |
| `setterm` | `setterm ble [default\|on\|off]` | Show or set the BLE preference for the next boot. |
| `setterm` | `setterm timezone [UTC\|UTC+/-offset\|Europe/Berlin\|POSIX-TZ]` | Show or set the timezone used for local time. |
| `setterm` | `setterm startup [auto\|flash\|sd]` | Show or select the volume containing `.shell/startup` for the next boot. |
| `setterm` | `setterm otaurl [url]` | Show or set the OTA metadata URL. |

Input completion lists every current source after `input test`, only absolute
pointer sources after `input calibrate`, common named keys after `input emit`,
`status` after an input class, and `set` or `reset` after a calibration source.
Gesture completion lists gesture-capable sources after `source=`, limits
`gesture=` values to the selected source's advertised gestures, and offers
`all` after `gesture unbind`.

### Native ELF modules

`load` is the low-level maintainer interface for the native-module runtime. It
is compiled only for ESP32-S3 boards with PSRAM. The command resolves the ELF
path through the calling shell's current directory, reads at most 2 MiB into
PSRAM, validates every ELF header and table boundary, checks for an Xtensa
ELF32 dynamic object, relocates it into executable PSRAM, and calls its entry
point with the supplied arguments.

Native modules import the single versioned `solar_os_native_host_v1` symbol.
The returned function table reports the ABI version, target, firmware version,
and provides UTF-8 output through the shell that invoked `load`. Its optional
`get_service` extension discovers versioned image, video, vision and inference
interfaces; see [Native media and inference](native-media.md). Existing modules
using the original output-only prefix remain compatible. Arbitrary SolarOS
internals are not exported through this interface. Native ELF code is not sandboxed; `load` is for trusted,
maintainer-produced modules only.

ELFs declaring lifecycle-2 app metadata launch through the
[foreground app runtime](native-app.md), retaining their image while suspended.
Installed apps launch by module name.
Ctrl+Alt+Del, Ctrl+] or closing the session requests cooperative shutdown.
The host restarts the device if shutdown does not finish within five seconds.

The ordinary `load` runner is for short-lived command-style modules. A module must stop
all of its work and release every callback and resource before its entry point
returns, because the runner immediately unloads its code and data. Installed
jobs, drivers, and libraries use separate resident lifecycle ABIs instead of this runner.

The standalone `modules/examples/hello` ESP-IDF project builds a small acceptance
module.

Official native-module catalogs are versioned with the host firmware. `pkg`
accepts a catalog only when its ECDSA signature, SolarOS version, ESP target,
and native ABI match the running firmware. Each downloaded ELF must match the
signed size and SHA-256, pass the same ELF validation as `load`, and is staged
before atomic activation. The signed type selects the storage and lifecycle
boundary: application modules use `/modules/apps/<module>.elf`, jobs use
`/modules/jobs`, drivers use `/modules/drivers`, and libraries use
`/modules/libraries`. Every entry separately
declares its lifecycle ABI and its type-specific native host API size.
Application lifecycle ABI 1 is the synchronous `main(argc, argv)`
run-and-unload contract. Job lifecycle ABI 1 registers start, stop, and tick
callbacks and keeps the ELF resident until the stopped job is removed. Driver
lifecycle ABI 1 registers a zero-binding utility driver contract and keeps the
ELF resident until all devices are detached and the module is removed.
Library lifecycle ABI 1 registers a versioned API table. Consumers acquire
leases that prevent unloading while calls, objects, or workers use that API.
Libraries load on demand from installed storage; `pkg unload ID` unloads an
unused library and retains its file. See [Native libraries](native-library.md).
Application lifecycle ABI 2 declares a foreground app manifest and runs
`main(argc, argv)` on its declared worker. Display, input, storage, timing and
optional audio are provided by the versioned foreground service. Resumable apps
retain their ELF and guest state across session switches.

Run `pkg` without arguments to open the package manager. It refreshes the
signed catalog in a worker, marks installed entries with `*`, and lets you
inspect, install, remove, or refresh modules without blocking screen redraws.
After a successful catalog check, `pkg install` completion uses that verified
catalog. `pkg remove` completion and `pkg installed` read the installed module
directories directly.

Installed application modules are shell commands: the module ID resolves under
`/modules/apps` after built-in commands, compiled applications, and aliases.
Command completion lists installed applications. `load` remains available for
explicit paths and diagnostics. Reinstalling an application from the schema-v1
layout moves it into `/modules/apps`; `pkg remove` also recognizes the legacy
flat application path during this transition.

The job and driver acceptance modules exercise their resident paths. Use `pkg install hello-job`,
then `job start hello-job`, `job status hello-job`, and `job stop hello-job`.
Use `pkg install hello-driver`, then `expansion attach hello-driver hello0` and
`expansion detach hello0`. The hello driver claims no GPIO or bus. `pkg remove`
rejects a running job or a driver with an attached device.

For example:

```text
pkg available
pkg installed
pkg install hello
hello SolarOS
pkg remove hello
```

### Power, RTC, schedules, and terminal settings

`power` usage:

```text
power status
power profile [performance|balanced|battery|lowpower]
power idle [off|seconds]
power key [short|long] [action]
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
display is suspended. It temporarily uses the `lowpower` profile and prevents the
idle policy from entering explicit light sleep. Another press of KEY,
a new keypress from a built-in or attached keyboard, or a BLE keyboard reconnect
resumes the display and restores the selected profile. Releasing a held key or
automatic key repeat does not resume the display. `power status` shows
the selected profile, effective profile, and suspend state.

`power key` is an alias for the KEY settings exposed by `setterm powerkey`.
Without `short` or `long`, an action changes the short press for compatibility.

`deepsleep` is a standalone command. It turns off the ESP32 radios and enters
ESP32 deep sleep after configuring the same board wake input, compatible RTC interrupt, and
next scheduled timer wake sources used by explicit light sleep. KEY, an armed
RTC or scheduled timer, and RESET start a normal fresh boot; running jobs,
sessions, and RAM state are not restored. BLE and Wi-Fi are off and cannot wake
the device. This is not a hardware power-off: the board remains powered, and a
separate board power control must still be used to remove its supply.

On Pi0cket CL-32, the built-in keyboard resumes suspend and wakes light or
deep sleep through the companion controller's shared interrupt. USB-power and
charging changes can also wake the board. Release held keys before sleeping.
Keyboard-equipped boards allow up to three seconds for held keys and the wake
interrupt to settle; sleep is cancelled if the input remains active.

`suspend` shows the SolarOS splash with **Suspended**; `sleep` and `deepsleep`
show **Sleeping**. The splash replaces the current contents on every attached
display before the transition, including e-paper displays that retain their
image. Waking from suspend or light sleep restores the previous session.
The CL-32 reflective LCD turns off during suspend after presenting the splash
and restores the selected display power policy on resume. Unlike e-paper, it
cannot retain the image after its supply is removed.

`shutdown` works from display and port shells on every target. It stops new
app and job launches, closes sessions, stops background jobs and radios,
flushes and unmounts persistent storage, and shows **Powered off** before
power-down. Save unsaved documents before shutdown; closing an app does not
automatically save edits. RAMFS contents and running sessions are discarded.
Cleanup failures are reported and prevent power-down. Use RESET or cycle power
to restart after a cleanup failure.

Shutdown uses the board's declared hardware power-off provider when available:
the Pi0cket CL-32 controller, AXP2101 on Waveshare ESP32-S3-ePaper-3.97, and BQ25896
battery disconnect on T-LoRa-Pager. Xteink X4 Pro drops its peripheral latch;
T-Deck Plus and ThinkNode M9 turn off their shared display/peripheral rail.
Other targets enter ESP32 deep sleep. USB power can keep a board
alive after its battery is disconnected; deep sleep is the fallback in that
case. Shutdown disables RTC and scheduled timer wake. Pressing the physical
power/KEY button starts a normal fresh boot, with apps and jobs initialized
from saved settings. A target without a usable KEY wake pin needs RESET or a
power cycle. Shutdown never resumes the old session.

`rtc` is the low-level hardware interface. `rtc status` remains useful on
boards without RTC hardware and reports `unavailable` there.

```text
rtc status
rtc alarm set HH:MM[:SS] [day=N] [weekday=N]
rtc alarm clear
rtc timer set <duration> [repeat]
rtc timer clear
rtc pending
rtc ack <alarm|timer|all>
```

Direct alarm and timer controls are leased. If the scheduler or a script owns
the requested hardware slot, the command reports the owner instead of replacing
its wake-up configuration.

`schedule` stores named alarms and script jobs in the internal flash filesystem
at `.solar/schedule.bin`. Updates replace the file atomically. Durations accept
`s`, `m`, `h`, or `d`; dates and times use configured local time.

```text
schedule list
schedule show <name>
schedule add <name> in <duration> <alarm|run script>
schedule add <name> every <duration> <alarm|run script>
schedule add <name> at YYYY-MM-DD HH:MM[:SS] <alarm|run script>
schedule add <name> daily HH:MM[:SS] <alarm|run script>
schedule add <name> weekly <sun,mon,...> HH:MM[:SS] <alarm|run script>
schedule enable <name>
schedule disable <name>
schedule remove <name>
schedule run <name>
schedule stop [name]
```

Only one scheduled shell script runs at a time. A due script is skipped and its
skip counter increases if another scheduled script is still running. Scheduled
scripts run without a terminal, and attempts to launch foreground applications
are rejected. Calendar schedules wait for valid wall-clock time. Interval
schedules continue to work from monotonic uptime without an RTC.

During explicit light sleep, the nearest schedule is armed as an internal timer.
When a wired interrupt-capable RTC is available, the scheduler also programs its
calendar alarm when wall-clock time is valid and, for a monotonic schedule, its
countdown timer. The RTC GPIO is added to the wake sources. This lets countdowns
use the RTC even while wall-clock time is invalid. An RTC interrupt can wake
light sleep; it cannot turn on a board whose hardware power has been switched
off.

`setterm` usage:

```text
setterm
setterm --display <target> [orientation|font|textsize|palette|statusbar] [value]
setterm orientation [0|90|180|270]
setterm font [mono|compact]
setterm textsize [8|10|12|14|16|18|20]
setterm palette [normal|inverted]
setterm foreground [#RRGGBB]
setterm background [#RRGGBB]
setterm statusbar [show|hide]
setterm brightness [0..100]
setterm backlight [0..100|follow]
setterm profile [vt100|ansi|dumb]
setterm charset [utf8|ascii]
setterm keyboard [us|de]
setterm powerkey [short|long] [default|off|enter|app-exit|sleep|suspend|deepsleep|ble-pair]
setterm keyrate [off|1..60 [delay-ms]]
setterm ble [default|on|off]
setterm timezone [UTC|UTC+/-offset|Europe/Berlin|POSIX-TZ]
setterm startup [auto|flash|sd]
setterm otaurl [url]
```

`setterm backlight` controls all registered auxiliary lights. It accepts a
percentage from 0 to 100 (0 turns them off), or `follow` to track the primary
display's configured brightness. Independent brightness defaults to 50%; both
the numeric setting and follow mode are saved across reboots. Follow mode
requires display brightness control. The keyboard backlights on T-Deck Plus,
T-LoRa-Pager, ThinkNode M9, and Pi0cket CL-32 use this control. Unsupported hardware
reports `unsupported`. Suspend and explicit sleep temporarily turn auxiliary
lighting off; resume restores the selected setting. Shutdown also turns it off.
The Pi0cket CL-32 companion controller retains its keyboard-activity timeout and fade.
T-Deck Alt+B toggles the light for the current boot without typing a character
or saving changes across reboots. Suspend and light sleep preserve this on/off
choice. Toggling on from a configured level of zero uses 50%. The query shows
the configured brightness, including while the light is toggled off.

`backlight` is no longer an alias for display brightness. Scripts that used
`setterm backlight` for the screen must use `setterm brightness` instead.

`setterm keyrate` configures the shared repeat policy for BLE, PS/2, CardKB,
and the Pi0cket CL-32 keyboard, fixed board buttons, `gpio-keys`, and ADC D-pads.
Analog joysticks publish axes and do not generate key events.
The value is stored in NVS and is available on builds without BLE.

`setterm ble` selects the next-boot BLE preference. `default` clears the saved
override and follows the active board profile; `on` and `off` remain in effect
across firmware updates until changed. The current boot is unchanged.

`setterm timezone` accepts fixed offsets with the conventional UTC sign:
`UTC-8` is eight hours behind UTC and `UTC+5:30` is five hours and 30 minutes
ahead. Fixed offsets do not apply daylight-saving transitions. Other accepted
timezone expressions use POSIX TZ syntax and its POSIX sign convention.
SolarOS does not include the IANA timezone database; `Europe/Berlin` is a
built-in daylight-saving alias.

`setterm powerkey` shows both effective KEY actions. Configure the gestures
independently, or use the `powerkey short` and `powerkey long` rows in the
`setterm` TUI:

```text
setterm powerkey short enter
setterm powerkey long suspend
setterm powerkey long sleep
setterm powerkey long deepsleep
setterm powerkey long ble-pair
setterm powerkey long default
```

The action choices are `default`, `off`, `enter`, `app-exit`, `sleep`, `suspend`,
`deepsleep`, and `ble-pair`. `off` disables the awake action; it does not cut
power or disable KEY wake. `sleep` enters explicit light sleep. `suspend`
turns off the display while jobs and services continue. `deepsleep` enters deep
sleep and starts a fresh boot on wake. `ble-pair` forgets the remembered BLE
keyboard and starts pairing when BLE is enabled for the current boot.

A hold is recognized after 1.2 seconds; light and deep sleep begin after KEY is
released. Any KEY press resumes suspend or wakes sleep,
regardless of its duration. The wake press is consumed until release, so it
cannot also type Enter, start pairing, or put the device back to sleep.

`default` restores the board's mapping. T-Deck defaults to Enter for a tap and
suspend for a hold. Other boards retain their normal short-press mapping; holds
default to suspend on targets with a board-owned wired keyboard and BLE pairing
on targets without one. Connecting a BLE keyboard does not change these defaults.
Valid saved choices take precedence over defaults, including older short-press
settings. Changes persist and apply immediately. `setterm powerkey sleep` remains
shorthand for `setterm powerkey short sleep`; `setterm key` is also accepted.

`setterm startup` selects the volume used for `.shell/startup` on the next boot.
The default `auto` setting prefers a mounted board-owned SD card and falls back
to flash when SD is unavailable. `flash` always selects internal flash, even
when an SD card is mounted. `sd` always selects SD and is rejected on boards
without SD support. Use `setterm startup` without a value to show the selected
source and currently resolved path.

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

`setterm --display <target>` reads or changes the volatile terminal profile of
a named runtime display target, even when a TUI application rather than a shell
owns that display. The target profile is initialized from the single global NVS
parameter set when the display registers. Orientation is relative to the
target's native panel rotation, so `0` keeps every display in its normal
mounting even when their drivers use different U8g2 rotations. Display-targeted
absolute pointer coordinates follow this logical orientation. A targeted change
applies to current and future sessions on that display until reboot or until the
display target is unregistered; it does not create or update per-display NVS
keys. Without a setting, the command prints the target's complete volatile
profile. For example:

```text
setterm --display oled0 statusbar hide
setterm --display oled0 textsize 10
setterm --display oled0 palette inverted
```

Text size `8` selects a fixed bitmap face with 6×8 pixel cells for both font
families. It uses the same upright glyphs for regular, bold, and italic text.
On a 240×64 display, hiding the status bar gives 38 columns and eight rows:

```text
setterm --display lcd1 textsize 8
setterm --display lcd1 statusbar hide
```

`foreground` and `background` select the persistent RGB theme colors for the
built-in color display. They color terminal scanout and semantic GUI elements,
including text, backgrounds, borders, and intermediate shades. Explicit RGB
image, canvas, and script colors remain literal. Use six hexadecimal digits,
for example `setterm foreground '#d8e8ff'` and `setterm background '#102030'`;
the leading `#` can be omitted. The defaults are `#000000` and `#ffffff`.
These settings do not add a color framebuffer or affect monochrome-display
rendering. `palette inverted` continues to exchange the foreground and
background roles.

`setterm statusbar hide` removes the top status bar from graphical shell
sessions and gives its space to the terminal. `show` restores it. The default is
`show` when no value is stored in NVS.

On BLE-capable builds, the status bar has a separate Bluetooth symbol: plain
means enabled for the current boot, slashed means disabled, and a small search
marker means scanning. The keyboard symbol shows only availability: plain when
at least one keyboard is available, slashed otherwise, regardless of transport.
Compact displays omit icons that do not fit before the clock and unread count.

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
The dumb profile can run line-oriented shell commands, but cursor-addressable
TUI applications cannot run on it and report
`<app>: can't run on a dumb terminal`.
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
job start graffiti
job start gestures
job start httpd /www
job start meshcore-ble 68:ee:8f:69:5f:35 public 123456
job start displayd [display-target]   # display0 by default, web0 when headless
job start cam-webd [qvga|vga] [fps] [auth=none|required]  # QVGA, 5 fps, no auth by default
job start rtspd [video=<stream>|none] [audio=<stream>|none] [size=qvga|vga] [fps=0..30] [port=<port>]
job start ntp-sync once
job start batmon 60
job start slip uart0 115200
job start pppd uart0 baud=115200
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
| `disk` | `disk mount <image.img> <mount> [partition]` | Mount a raw FAT image; select an MBR primary partition from 1 to 4 if needed. |
| `disk` | `disk create <image.img> <MiB>` | Create a new MBR/FAT hard-disk image without overwriting an existing file. |
| `disk` | `disk umount [flash\|sd0pN\|imgN\|mount]` | Unmount the default volume or an explicit volume/image/mount point. |
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
| `stream` | `stream tcp listen <name> <port>` | Register an exclusive duplex byte stream backed by a raw TCP listener. Requires TCP byte-stream support. |
| `stream` | `stream tcp connect <name> <host> <port>` | Register an exclusive duplex byte stream for an outbound TCP connection. |
| `stream` | `stream tcp remove <name>` | Remove an unclaimed TCP listener or outbound stream. |
| `stream` | `stream ble listen <name>` | Register an exclusive duplex BLE L2CAP CoC stream. Advertises when claimed. Requires BLE byte-stream support. |
| `stream` | `stream ble connect <name> <address> <public\|random\|rpa_public\|rpa_random>` | Register an outbound CoC stream. Connects when claimed. Use the address and type reported by `ble scan`. |
| `stream` | `stream ble status <name>` | Show connection state, negotiated SDU sizes, queued bytes, and connection interval. |
| `stream` | `stream ble remove <name>` | Remove an unclaimed BLE stream. |
| `model` | `model backend`; `model list`; `model info <handle>` | Check engine availability and inspect resident models, interfaces, memory, mode, and references. |
| `model` | `model load <file.espdl> [timeout_ms]`; `model bundle <bundle.json> [timeout_ms]` | Load a resident raw model or a checked model bundle from storage. |
| `model` | `model mode <handle> <single\|auto\|dual>` | Select execution mode without reloading the model. |
| `model` | `model unload <handle\|all>` | Explicitly unload models; active operations and retained model references report busy. |
| `pipeline` | `pipeline list` | List OS-owned native image pipelines. |
| `pipeline` | `pipeline start qr <source> [limit] [interval_ms]` | Decode QR codes from an image file, video stream ID, or JPEG RTSP URL in a native background job. |
| `pipeline` | `pipeline start model <source> <model> [limit] [interval_ms]` | Run a resident bundle with one image input. The job retains the model until processing ends or is stopped. |
| `pipeline` | `pipeline status <id>` | Show job state, frame counts, last error, and the latest frame timings. |
| `pipeline` | `pipeline result <id>` | Print the latest result as JSON. |
| `pipeline` | `pipeline stop <id>` | Cancel and join native workers, release the source and model reference, and keep the latest result. |
| `pipeline` | `pipeline destroy <id>` | Stop and remove the job, configuration, and latest result. |
| `daq` | `daq help` | Print DAQ usage. |
| `daq` | `daq status` | Show DAQ job status. |
| `daq` | `daq streams` | List stream IDs. |
| `daq` | `daq start <file.csv> <stream...> [--rate seconds\|--rate-ms ms]`; `daq start <stream...> <file.csv> [--rate seconds\|--rate-ms ms]` | Start periodic CSV capture from one or more streams. |
| `daq` | `daq start <file.csv> <stream> --changes [--append\|--replace]` | Capture a scalar or event stream only when its value changes. |
| `daq` | `daq start <file.bin> <byte-or-audio-stream> --raw [--rate-ms ms]` | Capture one byte or PCM audio stream without CSV framing. |
| `daq` | `daq stop` | Stop the active data-acquisition job. |
| `log` | `log status` | Show runtime log ring status. |
| `log` | `log show [count]` | Print recent SolarOS log entries. |
| `log` | `log follow [error|warn|info|debug]` | Follow logs in the current shell. |
| `log` | `log clear` | Clear the runtime log ring. |
| `log` | `log level [error|warn|info|debug]` | Show or change runtime log level. |
| `log` | `log sink cdc [on|off]` | Enable or disable CDC mirroring of SolarOS logs. |
| `port` | `port list` | List byte-stream ports. |
| `port` | `port status <name>` | Show port capabilities and owner. |
| `xfer` | `xfer protocols` | List supported and reserved transfer protocols. |
| `xfer` | `xfer send <port> <file> --raw [-d ms]` | Send a file as raw bytes, optionally delaying between chunks. |
| `xfer` | `xfer recv <port> <file> --raw [--append\|--replace] [--idle-ms ms]` | Receive raw bytes until the idle timeout and append or replace the destination. |
| `xfer` | `xfer send <port> <file> --zmodem` | Send a file with ZMODEM. |
| `xfer` | `xfer recv <port> <file> --zmodem [--append\|--replace]` | Receive a file with ZMODEM and append or replace the destination. |

TCP byte streams:

```text
stream tcp listen tcp0 7777
stream status tcp0
stream tcp remove tcp0
```

The optional TCP byte-stream package is included in the full flavor. It supports
up to four IPv4 TCP endpoints. Listeners bind to all network interfaces;
`stream tcp connect <name> <host> <port>` configures an outbound endpoint.
Hostnames resolve when the endpoint is registered; recreate it to resolve again.
The outbound socket connects lazily when a consumer first performs I/O. Each exposes one
exclusive duplex byte stream and port, without Telnet negotiation or newline
translation. TCP connections are unencrypted and unauthenticated; use a trusted
network. Endpoints are runtime settings and are not saved across reboot.

Start the stream consumer before connecting the client. I/O is nonblocking;
partial writes and backpressure are preserved. A disconnected peer causes an
I/O error for the current owner. Release and reopen the stream before attaching
a new peer; a new connection never continues the old owner's protocol session.

### BLE byte streams

The `service.ble-stream` package provides native BLE L2CAP Connection-Oriented
Channels (CoC). Up to four runtime endpoint configurations share at most two
active channels, subject to the host's connection budget. The OS reserves a
connection for its BLE keyboard. Configuration does not connect or advertise;
claiming the stream starts the connection, and releasing it closes the owned
BLE session and frees its data buffers. Endpoints remain until removed or
rebooted.

```text
stream ble listen ble0
dlp serve display0 stream=ble0
stream ble status ble0
```

On another device, run `ble scan`, then configure the discovered address and
type (`public`, `random`, `rpa_public`, or `rpa_random`):

```text
stream ble connect ble0 aa:bb:cc:dd:ee:ff public
expansion attach stream-dlp remote stream=ble0
```

The listener advertises `SolarOS <stream-name>` using the shared generic BLE
peripheral lease. Only one listener can be claimed at a time; it cannot run
alongside another generic GATT server or the HID peripheral role. The keyboard
input client can remain connected. Central/peripheral roles do not determine
which device sends data or serves a display.

Streams carry exact bytes in both directions, with no line conversion or Link
framing. Reads and writes are nonblocking after connection setup; a full transmit
queue returns a partial write. A paused reader withholds channel credits instead
of dropping data. The receiver and transmitter each have a bounded 4096-byte
queue per channel. CoC SDU boundaries are hidden from consumers.
Output queued before a peer connects stays with the current claim, allowing a
listening shell to send its initial banner when the central arrives. The receive MTU
fits one CoC PDU, so short SDUs cannot leave outstanding credits
without a receive buffer. `stream ble status` shows negotiated sizes and queues.

A disconnect fails the current claim. Release and reopen to create a fresh
connection; bytes from separate connections are never joined. DLP serving and
the `stream-dlp` driver handle this through their existing reconnect paths.
Open may wait for GAP/GATT connection setup. A central checks the SolarOS stream
service and version descriptor before opening PSM `0x0080`. This is native CoC,
not the GATT UART profile used by BLE serial applications. Streams currently
accept open connections and do not impose pairing, bonding, or peer authorization.

After releasing the consumer:

```text
stream ble remove ble0
```
Closing the stream closes its peer; the endpoint remains registered until
removed. An outbound endpoint attempts a new connection when reopened and used. Remove refuses a stream that is still claimed. Connected peers request
low-latency Wi-Fi and release that request when the connection closes.

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
| `network` | `network` | Open the two-tab network TUI. Status shows interfaces and routes; Settings changes persistent interface priority and client routing. |
| `network` | `network status` | Show network interfaces, route selection, VPN routes, and router state as text. |
| `network` | `network interfaces` | List route-capable interfaces, addresses, priorities, and the local downstream interface. |
| `network` | `network routes` | Show the default route, automatic base path, and active WireGuard routes. |
| `network router` | `network router [status\|on\|off]` | Route Wi-Fi AP clients through the SolarOS route table using IPv4 forwarding and NAT. |
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
| `wifi repeater` | `wifi repeater` | Show L2 IPv4 repeater state, upstream, downstream, learned clients, and forwarding counters. |
| `wifi repeater` | `wifi repeater on` | Repeat the current or preferred saved network with the same SSID and password on the same IPv4 subnet. |
| `wifi repeater` | `wifi repeater off` | Stop L2 forwarding and the downstream AP while retaining the upstream station. |
| `wifi ap` | `wifi ap [status]` | Show SoftAP status. |
| `wifi ap` | `wifi ap on [ssid [password [open|wpa|wpa2|wpa/wpa2]]]` | Start and save SoftAP settings. |
| `wifi ap` | `wifi ap off` | Stop SoftAP. |
| `wifi nat` | `wifi nat [status|on|off]` | Advanced control for IPv4 NAT on the SoftAP interface. Prefer `network router`. |
| `wireguard` | `wireguard [status]` | Show profile, tunnel, selected underlay, route, peer, DNS, and kill-switch state without printing key material. |
| `wireguard` | `wireguard import <file>` | Validate one standard WireGuard client profile and save it in NVS. The source file is not removed. |
| `wireguard` | `wireguard forget` | Logically remove the saved profile from NVS. Bring the tunnel down first. |
| `wireguard` | `wireguard up [fail-open\|fail-closed]` | Request the tunnel and reconnect it when the preferred network underlay changes. The default is fail-closed for a full tunnel and fail-open for a split tunnel. |
| `wireguard` | `wireguard down` | Stop the tunnel, remove its routes, restore DNS, and disable its kill switch. |
| `ble` | `ble [status]` | Show BLE keyboard state and the current/next boot setting. |
| `ble` | `ble enable` | Save BLE enabled for the next boot. The current boot is unchanged. |
| `ble` | `ble disable` | Save BLE disabled for the next boot. The current boot is unchanged. |
| `ble` | `ble default` | Clear the saved override and use the board default on the next boot. |
| `ble` | `ble keepalive [on\|off]` | Show or save the best-effort BLE keyboard keepalive setting. |
| `ble` | `ble scan` | Scan nearby BLE devices. |
| `ble` | `ble pair` | Forget the remembered keyboard and its bond, then start pairing a replacement. |
| `ble` | `ble forget` | Erase the remembered keyboard, its BLE bond, and its cached GATT service database. |
| `ble gatt` | `ble gatt status` | Show the generic GATT connection state and discovered-service count. |
| `ble gatt` | `ble gatt connect <aa:bb:cc:dd:ee:ff> <public\|random\|rpa_public\|rpa_random>` | Connect to a BLE peripheral by address and address type. |
| `ble gatt` | `ble gatt pair <six-digit-pin>` | Secure and bond the connected GATT peripheral with its six-digit PIN. |
| `ble gatt` | `ble gatt disconnect` | Request disconnect of the shell's GATT session and cancel its pending operation. |
| `ble gatt` | `ble gatt services` | List discovered services and their indexes and handle ranges. |
| `ble gatt` | `ble gatt chars <service-index>` | List the characteristics discovered for one service. |
| `ble gatt` | `ble gatt read <handle>` | Read a characteristic or descriptor by handle. |
| `ble gatt` | `ble gatt write <handle> <hex...>` | Write hexadecimal bytes and request a response. |
| `ble gatt` | `ble gatt write-nr <handle> <hex...>` | Write hexadecimal bytes without requesting a response. |
| `mqtt` | `mqtt status` | Show broker, authentication, connection, traffic, queue, and error status without revealing the password. |
| `mqtt` | `mqtt connect [mqtt[s]://host[:port] [username [password]]]` | Connect to a broker and save supplied connection settings; omit them to reuse saved settings. |
| `mqtt` | `mqtt disconnect` | Disconnect and stop the MQTT client. |
| `mqtt` | `mqtt publish <topic> <payload> [qos] [retain]` | Publish a message with optional QoS 0–2 and retain flag. |
| `mqtt` | `mqtt subscribe <topic> [qos]` | Subscribe and print received messages until app-exit or `q`. |
| `ping` | `ping <host> [count]` | Send ICMP echo requests. Without count, ping runs until Esc, Ctrl+C, or app-exit. |
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

When no saved preference exists, including after `nvs clear`, BLE follows the
board default. Most boards enable it; TTGO VGA32 v1.4 disables it to preserve
internal heap. `setterm ble on|off|default` stores or clears the preference for
the next boot. `ble enable`, `ble disable`, and `ble default` are equivalent
compatibility commands. Disabling BLE does not forget the remembered keyboard
or erase its BLE bond. On a BLE-disabled boot, SolarOS returns the unused
Bluetooth controller and host memory to the internal heap before normal service
initialization.

Discovery through `ble scan`, the BLE Inspector, or the scripting APIs temporarily
pauses automatic keyboard reconnect attempts. Reconnect resumes after the scan.
An existing keyboard connection remains open.

When a keyboard exposes the standard Battery Service, SolarOS subscribes to its
Battery Level notifications and reads the initial level when supported. `ble
status`, the general `status` command, and the Python/Lua BLE status strings
include the latest percentage while that keyboard remains connected.

### BLE inspector TUI

Run `ble` without arguments to open the BLE Inspector TUI. Its Devices,
Services, Chars, and Settings tabs support scanning, connecting, service and
characteristic inspection, characteristic reads and hexadecimal writes, and
BLE keyboard settings. Press Tab to move between tabs. The inspector owns its
GATT connection and disconnects it when the TUI closes. Existing `ble status`,
`ble scan`, and `ble gatt ...` commands remain available for scripts and plain
text use. On a BLE-disabled boot, the Settings tab remains available so BLE can
be enabled for the next boot.

### BLE keepalive and GATT

`ble keepalive on` sends the standard HID Exit Suspend command every 30 seconds
when the connected keyboard exposes a writable HID Control Point. It falls back
to reading the HID Information characteristic when Exit Suspend is unavailable.
The setting persists and is off by default. `ble keepalive` reports the selected
method, the last local operation result, and the attempt count. These operations
do not count as SolarOS user activity. They can prevent sleep when a keyboard
bases its inactivity timer on host HID or GATT traffic, but the keyboard remains
in control of its own power policy, so the result is device-specific. `ble
keepalive off` stops the operations.

BLE GATT usage:

```text
ble gatt status
ble gatt connect <aa:bb:cc:dd:ee:ff> <public|random|rpa_public|rpa_random>
ble gatt pair <six-digit-pin>
ble gatt disconnect
ble gatt services
ble gatt chars <service-index>
ble gatt read <handle>
ble gatt write <handle> <hex...>
ble gatt write-nr <handle> <hex...>
```

The shell GATT session owns one peer and shares the configured connection budget
with app BLE sessions, which may own multiple peers. It cannot read or disconnect
an app-owned connection. Disconnect and
operation timeout retire the connection asynchronously; reconnect can report
busy until cleanup finishes. A write without response waits for local stack
completion, not a remote acknowledgement. Generic scans are unavailable while
any generic peer is active or retiring.

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
| `battery` | `battery [status]` | Show voltage, state of charge, power source, config, and monitor trend. |
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
| `say` | `say [-v <0..100>] [--volume <0..100>] [--pitch <50..200>] [--speed <20..500>] [--drop-if-busy] (--file <path> \| [--] <text...>)` | Queue text for offline speech, or stream a plain UTF-8 text file aloud with live progress. Pitch and speed default to 100. File mode submits bounded chunks through one continuous PicoTTS/audio session, completes paths with Tab, and remains responsive; press Esc or Ctrl+C to stop. Start `speechd` with a PicoTTS voice directory first. |
| `led` | `led [status|on|off|toggle]` | Inspect or control the built-in status LED when available. |
| `camera` | `camera [status]` | Initialize the camera on first use and show the detected sensor, JPEG configuration, current owner and frame lease, capture count, and last error. |
| `camera` | `camera capture <path> [qvga|vga]` | Capture one JPEG into a caller-selected SolarOS path. The default is QVGA; changing size reinitializes the idle camera. Existing files are replaced. |
| `camera` | `camera off` | Deinitialize an idle camera and release its framebuffer and driver resources. |
| `expansion` | `expansion` | Open the expansion device manager. Browse attached devices and driver categories, inspect details, attach supported drivers, save runtime attachments to the selected startup script, and detach runtime devices. Bus lifecycle remains in the `io` app. |
| `expansion` | `expansion status` | Show expansion capabilities, named buses and leases, connector resources, active devices, and resource claims. |
| `expansion` | `expansion layout [connector]` | Draw the board's physical connector map with live free, releasable, claimed, fixed, power, ground, and NC markers. |
| `expansion` | `expansion scan` | List expansion resources and probe-capable drivers. |
| `expansion` | `expansion drivers` | List registered expansion drivers. |
| `expansion` | `expansion devices` | List fixed board and runtime-attached expansion devices with origin, readiness, startup mode, policy, and bindings. |
| `expansion` | `expansion bus create i2c <name> port=<i2c0\|i2c1> sda=<gpio> scl=<gpio> [speed=<hz>]` | Define a runtime I2C bus on an unused controller and approved expansion pins. |
| `expansion` | `expansion bus create onewire <name> pin=<gpio>` | Define a runtime named 1-Wire bus on an approved expansion pin. |
| `expansion` | `expansion bus create ps2 <name> clock=<gpio> data=<gpio>` | Define an exclusive PS/2 bus on two approved expansion pins. |
| `expansion` | `expansion bus create midi <name> tx=<gpio> rx=<gpio> [baud=<rate>]` | Define an exclusive MIDI bus; SolarOS selects the UART backend and defaults to 31250 baud. |
| `expansion` | `expansion bus create spi <name> host=<spi2\|spi3> sclk=<gpio> mosi=<gpio> [miso=<gpio\|none>] cs=<gpio> [cs=<gpio> ...] [max=<bytes>]` | Define a runtime-routed SPI bus on a board-approved host and expansion pins. |
| `expansion` | `expansion bus create uart <name> port=<uart1\|uart2> tx=<gpio> rx=<gpio> [baud=<rate>]` | Define a lazy runtime UART on an unused controller and approved expansion pins. |
| `expansion` | `expansion bus attach <name>` | Attach a named detachable bus and reserve its endpoint and signal pins. |
| `expansion` | `expansion bus detach <name>` | Detach an idle named bus, preserving its descriptor while releasing its endpoint and signal pins. |
| `expansion` | `expansion bus remove <name>` | Remove an idle runtime bus and release its signal pins. |
| `expansion` | `expansion attach <driver> <name> <resource...>` | Attach a registered expansion driver or manual resource profile. |
| `expansion` | `expansion detach <name>` | Detach an active expansion device and release its resource claims. |
| `expansion` | `expansion attach <board-device-name>` | Restore a detached board device with its manifest bindings and board origin. |
| `expansion` | `expansion export <path>` | Atomically export runtime buses and catalog-backed device attachments as a portable expansion manifest for custom-board generation. |
| `neopixel` | `neopixel [status\|list] [name]` | List attached WS2812/NeoPixel strips. |
| `neopixel` | `neopixel set <name> <index> <red> <green> <blue>` | Set one zero-based pixel and immediately refresh the strip. |
| `neopixel` | `neopixel fill <name> <red> <green> <blue>` | Fill and immediately refresh the strip. Color components are `0..255`. |
| `neopixel` | `neopixel clear\|show <name>` | Clear a strip immediately, or transmit its buffered colors. |
| `midi` | `midi status` | Show MIDI worker, traffic, parser, and queue status. |
| `midi` | `midi monitor` | Print incoming CC and key messages until the app-exit key, `Esc`, or `q` is pressed. |
| `midi` | `midi note-on\|note-off <channel> <note> [velocity]` | Queue a MIDI note message for transmission. |
| `midi` | `midi cc <channel> <controller> <value>` | Queue a MIDI control-change message. |
| `midi` | `midi program <channel> <program>` | Queue a MIDI program-change message. |
| `midi` | `midi send <status> [data1] [data2]` | Queue one validated raw MIDI message. |
| `midi` | `midi stream list` | List configured incoming MIDI CC scalar streams and their latest values. |
| `midi` | `midi stream add\|remove <channel> <controller>` | Register or remove `midi.cc.<channel>.<controller>` as a scalar stream. |
| `midi` | `midi stream clear` | Remove all configured MIDI CC scalar streams. |
| `control` | `control list\|parameters\|bindings` | Inspect normalized controls, native app parameters, or target bindings. |
| `control` | `control create <name> <stream> <min> <max> [smooth=ms] [deadband=value] [invert]` | Normalize a scalar stream as a named continuous control; use `manual` for script-supplied values. |
| `control` | `control bind <name> parameter <path> [pickup=on\|off]` | Bind a control to a typed native-app parameter with optional soft takeover. |
| `control` | `control bind <name> midi <channel> <cc>` | Bind a control to a MIDI Control Change target. |
| `control` | `control get\|set <name> [value]` | Read or set a normalized `0..65535` control value. |
| `control` | `control parameter get\|set <path> [value]` | Read or set an available native parameter in its declared unit. |
| `control` | `control unbind <name>` | Remove all target bindings owned by one named control. |
| `control` | `control delete <name>` or `control clear` | Remove one control and its bindings, or remove all controls and bindings. |
| `osc` | `osc bindings` | Inspect named outbound OSC bindings and their live source, value, send, and error state. |
| `osc` | `osc bind <name> stream <stream> <address> [rate=hz] [delta=value] [send=change\|always]` | Publish one scalar stream as OSC float32 values in its native unit. |
| `osc` | `osc bind <name> stream <event-stream> <address> edge=rising\|falling\|both [rate=hz]` | Publish sampled boolean transitions as OSC int32 `0` or `1`. |
| `osc` | `osc bind <name> control <control> <address> [rate=hz] [send=change\|always]` | Publish one normalized named control as an OSC float32 value from `0.0..1.0`. |
| `osc` | `osc unbind <name>` or `osc clear` | Remove one outbound binding or all outbound bindings. |
| `radio` | `radio` | Open the packet-radio TUI with live status and editable common config. |
| `radio` | `radio scan [name]` | Open the graphical `rfscan` app when included in the firmware. |
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
| `meshcore` | `meshcore channel list\|add\|remove\|public` | Join public hashtag channels or manage bounded shared-key groups while the job is stopped. |
| `meshcore` | `meshcore stream list` or `meshcore stream status [port]` | Inspect trusted peer-bound virtual serial ports carried by encrypted MeshCore direct packets. |
| `meshcore` | `meshcore stream create <port> <trusted-endpoint-id>` | Register a reliable MeshCore virtual serial port for one exact trusted endpoint. Configure both peers. |
| `meshcore` | `meshcore stream remove <port>` | Remove an unclaimed MeshCore virtual serial port. |
| `radio` | `radio state <name> [sleep|standby|rx|tx]` | Show or change radio operating state. |
| `radio` | `radio send <name> <text|byte...>` | Send one packet. |
| `radio` | `radio recv <name> [timeout-ms]` | Receive one packet and print metadata plus payload. |
| `gnss` | `gnss [list]` | List registered GNSS receivers and their concrete drivers. |
| `gnss` | `gnss status [name]` | Show power, fix, fix type, and satellite state for one logical receiver. |
| `gnss` | `gnss enable [name]` / `gnss disable [name]` | Enable or silence NMEA output on a receiver with recognized software configuration support. Power and UART ownership remain unchanged. |
| `gnss` | `gnss power <on\|off> [name]` | Enable or disable one logical receiver using its driver-specific power control. |
| `gnss` | `gnss fix [name] [timeout-ms]` | Poll one receiver for a position, UTC time, fix type, satellite count, and accuracy. A timeout can be supplied without a receiver name. |
| `modem` | `modem` | Open the modem status and settings TUI. |
| `modem` | `modem list` | List registered cellular modems, concrete drivers, transports, and power, reset, and baud capabilities. |
| `modem` | `modem status [name]` | Read SIM readiness, LTE registration, signal, packet-context, and IP-interface state. |
| `modem` | `modem power <on\|off> [name]` | Switch a modem's optional hardware power line. |
| `modem` | `modem reset [name]` | Reset a supported modem using its reset line or driver-defined power cycle. |
| `modem` | `modem baud [name] [auto\|rate]` | Show or persist an optional serial modem transport rate. `auto` selects the driver's preferred rate. |
| `modem` | `modem profile set [name] --apn <apn> [--dns <ipv4\|auto>] [--ip ipv4\|ipv6\|ipv4v6] [--auth none\|pap\|chap\|auto] [--user <user> --password <password>]` | Validate, apply, and persist a modem-independent cellular profile. |
| `modem` | `modem profile show\|clear [name]` | Inspect a saved profile with its password redacted, or remove it. |
| `modem` | `modem connect\|disconnect [name]` | Bring the saved cellular network connection and PPP interface up or down. |
| `modem` | `modem sim unlock <pin> [name]` | Enter a required SIM PIN without persisting it. |
| `modem` | `modem at <quoted-command> [name] [timeout-ms]` | Send one validated AT command and print the complete response. |
| `can` | `can [list]` | List the attached classic CAN controller and bitrate. See [Classic CAN](hardware.can.md). |
| `can` | `can status <name>` | Show CAN state, queues, and error counters. |
| `can` | `can send <name> <id> <hex-or-> [ext] [rtr]` | Queue a standard, extended, or remote CAN frame with up to eight bytes. |
| `can` | `can recv <name> [timeout-ms]` | Read one CAN frame, waiting at most 1000 ms. |
| `can` | `can recover <name>` / `can start <name>` | Start bus-off recovery, or restart after recovery reaches stopped state. |
| `haptic` | `haptic [list]` | List registered haptic devices, their concrete drivers, and supported effect range. |
| `haptic` | `haptic play <effect> [name]` | Play one numbered effect on a haptic device. |
| `haptic` | `haptic stop [name]` | Stop the active haptic effect. |
| `encoder` | `encoder [list]` | List named encoders, providers, and counting modes. |
| `encoder` | `encoder status <name>` | Read the mode and signed count of one encoder. |
| `encoder` | `encoder read <name>` | Print the signed count without clearing it. |
| `encoder` | `encoder reset <name>` | Set the encoder count to zero. |
| `motor` | `motor [list]` | List registered motor endpoints, drivers, and supported operations. |
| `motor` | `motor status <name>` | Read mode, normalized applied drive, common fault masks and optional driver diagnostics. |
| `motor` | `motor drive <name> <value>` | Apply a normalized signed demand from `-1.0` to `1.0`; zero coasts. |
| `motor` | `motor coast\|brake\|clear <name>` | Coast, brake, or coast and clear fault latches. |
| `charger` | `charger [list]` | List registered battery chargers, concrete drivers, and valid configuration ranges. |
| `charger` | `charger status [name]` | Show charger state, input status, configured limits, and the raw fault byte. |
| `charger` | `charger enable <on\|off> [name]` | Enable or disable charging. |
| `charger` | `charger input-limit <mA> [name]` | Set the input current limit to an advertised, exact step value. |
| `charger` | `charger current <mA> [name]` | Set fast-charge current to an advertised, exact step value. |
| `charger` | `charger voltage <mV> [name]` | Set charge voltage to an advertised, exact step value. |
| `imu` | `imu [list]` | List registered motion sensors, their concrete drivers, and available measurements. |
| `imu` | `imu sample [name] [timeout-ms]` | Read acceleration, angular velocity, and optional orientation from one motion sensor. |
| `nfc` | `nfc [list]` | List registered NFC readers and their concrete drivers. |
| `nfc` | `nfc power <on\|off> [name]` | Enable or disable a reader that has a driver-managed power rail. |
| `nfc` | `nfc scan [name] [timeout-ms]` | Discover one collision-free NFC-A tag and print its UID, ATQA, and SAK. |
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
| `pwm` | `pwm providers` | List registered PWM controllers. |
| `pwm` | `pwm status` | Show named outputs and their frequency groups. |
| `pwm` | `pwm set <output> <freq-hz> <duty-percent>` | Start PWM on provider:number, or a native GPIO number. |
| `pwm` | `pwm pulse <output> <freq-hz> <pulse-us>` | Command a pulse width in microseconds. |
| `pwm` | `pwm off <output>` | Stop a general PWM output. |
| `servo` | `servo list` | Show names, outputs, enabled state, and owners in a compact list. |
| `servo` | `servo status [name]` | Show calibration, frequency, inversion, and the last accepted command for all servos or one named servo. |
| `servo` | `servo open <name> <output> <min-us> <max-us> <min-deg> <max-deg> [freq-hz [inverted:0\|1]]` | Reserve an output without pulses; see [Servo outputs](servo.md). |
| `servo` | `servo set <name> <angle-deg>` / `servo pulse <name> <pulse-us>` | Command a calibrated angle or pulse width. |
| `servo` | `servo disable <name>` / `servo close <name>` | Stop pulses, retaining or releasing the reservation. |
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
| `temperature` | `temperature [list\|sensor]` | List temperature providers or read the default or named sensor. |
| `humidity` | `humidity [list\|sensor]` | List humidity providers or read the default or named sensor. |

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
one setting.

`display relax display0` runs black/white recovery for 60 seconds. Supply a
duration from 1 to 86400 seconds, for example `display relax display0 3600` for
one hour. It alternates solid black and white using synchronous native full
refreshes, with up to one second of hold after each refresh. It completes a
black/white pair when the time expires; a hardware refresh can extend the run
beyond the requested duration. The app-exit key (normally `Ctrl+]`), `Ctrl+C`,
or Escape stops it between refreshes. The command returns to the shell after
restoring the saved image and previous refresh mode, including pulse tuning.
Sleep and shutdown interrupt recovery before the power transition.

Recovery is available on all built-in EPD drivers: every SSD1683 profile,
SSD1685, SSD1677, UC8179, and UC8279, including the X4 display wrapper and
runtime-attached panels. LCD, RLCD, and other display types return unsupported.
It claims the whole physical target; close apps
or remove layouts that own it first. A display-shell session can recover its
own target without closing that session. Ordinary presentation and pulse scans
are suspended during recovery. Completion reports black/white cycles, not a
measurement of ghost removal; inspect the physical panel afterward.

`display join wall0 --horizontal display0 lcd0` claims two to four backing
targets and registers `wall0` as one logical target. Horizontal joins place
targets from left to right; vertical joins place them from top to bottom. The
largest cross-axis dimension defines the logical canvas and unused backing
area is cleared. `display split display0 --horizontal left0 right0` claims one
backing target and registers two equal logical viewports. On a 792x272
CrowPanel this creates two 396x272 targets. Each viewport has its own display
buffer, terminal profile, session ownership, and frame-export surface; updating
one viewport preserves the other.

Layouts are runtime-only and are shown by `display layouts`. Backing targets
remain visible in `display list` with an owner such as
`display-layout:display0`, while logical targets have source `layout` and role
`joined` or `viewport`. Layout targets work with normal commands such as
`session create shell left0` and `session create files right0`. A layout cannot
use another layout as backing, does not scale content, and cannot be removed
while any logical target is owned or exported. When the built-in display shell
creates a split, session 0 automatically moves to the first viewport; when it
creates a join containing its current display, it moves to the joined target.
Removing that layout moves session 0 back to its original board display. Shell
state and scrollback are preserved while terminal geometry is recalculated for
the new target. Layout updates are coalesced and backing targets are refreshed
sequentially, which is especially important for e-paper panels. `display
unjoin wall0` and `display unsplit display0` release the backing targets.

With `power=auto`, the built-in ST7305 path uses the normal power
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
On SSD1683 board and expansion targets, `refresh=auto` starts with a full
waveform and skips unchanged frames. Waveshare V2 expansion targets use the
changed framebuffer rectangle and the controller's partial-window waveform,
with a full cleanup after every 19 partial updates. The Elecrow 5.79-inch dual
controller transfers both RAM halves with its `0xDC` partial waveform after the
initial cleanup and does not schedule periodic full refreshes. `refresh=fast`
forces a fast full-frame waveform and `refresh=full` forces the full cleanup
waveform on every changed frame.

SSD1685 targets default to asynchronous `refresh=pulse` and also provide
factory `refresh=auto|partial|full` modes. In auto mode,
`cleanup-every=<1..128>` sets the number of partial updates before a full OTP
cleanup (default 64). `refresh=partial` bypasses periodic cleanup;
`refresh=fast` is an alias for it.

The Elecrow 5.79-inch dual-SSD1683, Elecrow 4.2-inch green-sticker,
Waveshare 4.2-inch V2/revision-2.2, and SSD1685 GDEY029T71H profiles provide the
`refresh=pulse` monochrome mode. `display mode <target> drive-cycles=<1..32>`
sets the per-pixel budget, `pulse-frames=<1..32>` sets each drive phase's scan
duration, `waveform-fr=<1..12>` selects a raw frame-rate code (the dual SSD1683
accepts only 1–5 or 9–12), and
`cleanup-every=<1..128>` sets the cleanup baseline.
`cleanup-policy=adaptive` tracks actual successful drives per pixel.
It cleans up when the most-driven pixel reaches `cleanup-every * drive-cycles`
activations, or after `4 * cleanup-every` changed images, whichever comes first.
`cleanup-policy=fixed` cleans up every `cleanup-every` changed images.
Synchronous mode counts completed changed presentations; asynchronous mode
counts changed targets consumed by the worker. Identical targets do not advance
the image count. At a limit, the asynchronous worker schedules a native cleanup
without waiting for another presentation or for all pending pixels to settle.
Mode output includes the policy, peak/limit drive cycles, and changed-image age.
These counters estimate accumulated activity and do not measure ghosting.
These runtime settings apply to pulse mode, with SSD1685's `cleanup-every`
also controlling its native auto-mode cleanup interval. They are listed by
`display mode <target>`. `pulse-scan=sync` completes a presentation's pending
cycles before returning. `pulse-scan=async` runs one cycle per scan in a
persistent worker and accepts new targets while earlier pixels settle.
`pulse-period=<10..1000>` requests the minimum new-image interval in milliseconds;
the dual-SSD1683, Elecrow green-sticker, and SSD1685 profiles instead accept 67–1000 ms.
Settling drives run as BUSY permits without consuming new-image slots. A new
target can replace the current target between drives when its image slot is due;
unchanged pixels keep their remaining budgets. Identical submissions do not
consume image slots. Synchronous operation still completes all pending drives
before returning and applies the requested interval between changed targets.
Mode output separates `frame-count` and `frame-interval-us` for distinct target
snapshots from `scan-count` and `scan-interval-us` for physical drives. The frame
count includes native baseline snapshots; submitted/coalesced counts also appear.
`pulse-power=<cycle,hold>` selects supply cycling or retention during settling,
`pulse-ramp=<normal,fast>` selects booster duration, and
`pulse-spi-khz=<100..20000>` overrides the SPI clock only for pulse operation.
SSD1685 limits pulse SPI to 100–10000 kHz and also exposes `invert=<false,true>`
for polarity testing; its custom waveform requires panel validation. See the
[SSD1685 reference](expansion.reference.md#gdey029t71h-e-paper-ssd1685) for defaults.
Elecrow 5.79-inch defaults are asynchronous, 67 ms new-image interval, retained supplies, fast
booster ramp, 20000 kHz pulse SPI, 6 drive cycles, 8 scan frames, FR code 4,
and adaptive cleanup with a baseline of 64. This profile uses the Fastwave
waveform builder on both controllers, reported as `waveform=fastwave`, with
no reset kick and one VSS settle frame after the configured drive phase.
Grayscale images use the existing dithering path.
CrowPanel 5.79-inch and SolarWriter
start in pulse mode, with native auto as the fallback if the worker cannot start. Requested
image cadence is not a guarantee of achieved cadence or optical settling.
Waveshare 4.2-inch defaults are asynchronous, 100 ms new-image interval,
supply cycling, normal ramp, the configured SPI clock, 3 drive cycles,
8 scan frames, FR code 4, and adaptive cleanup with baseline 64. Native and
pulse timing logs support comparison on that panel.
The Elecrow green-sticker profile adapts the Fastwave phase builder to its
UC8276 OLD/NEW RAM and programmable waveform registers, reported as
`waveform=fastwave-uc8276`. It retains the controller's voltage and PLL setup,
uploads both full RAM masks, and leaves unbudgeted pixels undriven. Its defaults
are asynchronous, 67 ms new-image interval, supply cycling, fast ramp,
20000 kHz pulse SPI, 8 drive cycles,
8 scan frames, raw PLL code 12, and fixed cleanup every 8 changed images.
The Elecrow 4.2-inch board starts this revision in pulse mode. For this profile,
`waveform-fr` selects a raw PLL code; internal scan frequency is separate from
image update rate.
The mode requires physical panel calibration; other EPD
profiles reject it until they have a matching waveform adapter. See the
[SolarWriter display controls](boards.md#solarwriter) and
[Waveshare 4G E-paper](boards.md#waveshare-4g-e-paper) for behavior and recovery.

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

`rx-gain` controls receiver gain policy. `radio status <name>` lists the modes
supported by that radio; the radio TUI's `rx gain` row cycles through those
choices with Left/Right. Changes leave the radio in standby, like other radio
configuration changes.

| Driver | RX gain modes | Default |
| --- | --- | --- |
| SX1262 | `power-saving`, `boosted` | `power-saving` |
| RFM95W | `power-saving`, `boosted` | `boosted` |
| RFM69 / RFM69H | `auto`, `g1` through `g6` | `auto` |

```text
radio config radio0 rx-gain boosted
radio state radio0 rx
```

On SX1262 and RFM95W, `boosted` enables the higher-current RX/LNA mode;
`power-saving` disables that boost. Automatic gain control remains active.
On RFM69, `auto` enables AGC; `g1` selects maximum LNA gain and `g2` through
`g6` reduce it by 6, 12, 24, 36, and 48 dB respectively. Fixed gain can reduce
the receiver's ability to handle strong signals. These settings are not
transmit power or a common numeric gain scale across radio families.
`default` restores the driver's default policy. Unsupported modes are rejected.

Profiles avoid partially reconfiguring one end while copying a list of fields.
`lora-eu868`, `gfsk-eu868`, and `ook-eu868` are built in. Up to eight user
profiles, including RX gain, are saved in mounted-storage settings and consume
no idle profile cache. Existing profiles without RX gain use the driver's
default:

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
hello SolarOS
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
