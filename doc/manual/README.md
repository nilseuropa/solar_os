# SolarOS User Manual

This is the canonical documentation used by GitHub, the generated solar-os.eu website, the signed on-device `help` browser, `man`, and the native agent reference tool.

## Getting started

- [Browsing and refreshing documentation](help.md) — Browse the manual and refresh its signed Markdown pages
- [SolarOS manual](overview.md) — Find commands, applications, jobs, scripting APIs, and hardware concepts
- [SolarOS scripting conventions](script.conventions.md) — Write cooperative Python and Lua programs against SolarOS services

## Shell and storage

- [Shell command reference](commands.md) — Complete syntax, behavior, and examples for built-in shell commands
- [Storage and shell paths](storage.md) — Use SolarOS volumes, files, directories, and shell-style paths

## Commands

- [adc command](commands.md) — Show ADC service status.
- [agent command](agent.md) — Open a new native LLM agent TUI or make one unsaved foreground request.
- [apps command](commands.md) — List registered foreground apps compiled into the firmware.
- [audio command](commands.md) — Show audio state, global speaker level, tone queue, and active synth telemetry.
- [battery command](commands.md) — Show voltage, estimated charge, power source, config, and monitor trend.
- [ble command](commands.md) — Show BLE keyboard state and the current/next boot setting.
- [board command](commands.md) — Print board ID, name, and capabilities.
- [cat command](commands.md) — Print a small text file.
- [cd command](commands.md) — Change current shell directory.
- [clear command](commands.md) — Clear the active shell terminal.
- [close command](commands.md) — Close a display app, display shell, or retained port app, or stop a port shell session. The final interactive shell cannot be closed.
- [commands command](commands.md) — List built-in shell commands.
- [contacts command](commands.md) — Open the searchable provider-neutral contact browser.
- [control command](commands.md) — Inspect normalized controls, native app parameters, or target bindings.
- [cp command](commands.md) — Copy a file or matched set.
- [daq command](commands.md) — Print DAQ usage.
- [date command](commands.md) — Show or set the local date.
- [df command](commands.md) — Show free space on mounted storage volumes.
- [disk command](commands.md) — Show persistent-storage status.
- [display command](commands.md) — List drawable display targets, draw a test pattern, or change driver-specific display settings.
- [dpad command](commands.md) — Show ADC D-pad pins, raw values, zones, and calibration thresholds.
- [echo command](commands.md) — Print the arguments separated by spaces, followed by a newline. Quotes preserve spaces and are not printed.
- [email command](commands.md) — Open the receive-only email app.
- [engine command](commands.md) — Print or reset generic engine utilization counters for CPU/SIMD-style backends and vector bulk operations.
- [espnow command](commands.md) — Show ESP-NOW owner, channel, PHY, peers, traffic, drops, conflicts, and last error.
- [exit command](commands.md) — Close the current UART, USB CDC, or telnet shell when another interactive shell remains.
- [expansion command](expansion.md) — Show expansion capabilities, named buses and leases, connector resources, active devices, and resource claims.
- [fg command](commands.md) — Resume a display session or a port-owned app on its owning terminal. Without an ID, restore the calling port shell's most recently suspended app.
- [gateway command](commands.md) — Show gateway configuration, connection state, and traffic counters.
- [gpio command](commands.md) — List board GPIOs with free, releasable, or fixed pin policy.
- [help command](help.md) — Browse the package-aware manual or manage its signed exact-version SD copy.
- [humidity command](commands.md) — Read the board humidity sensor when available.
- [i2c command](commands.md) — Show every named I2C bus, or one selected bus.
- [identity command](identity.md) — Show the configured user and hostname.
- [inbox command](commands.md) — Open the universal incoming-message browser.
- [input command](commands.md) — List all input sources or filter them by semantic class.
- [job command](jobs.md) — Show one job or all jobs.
- [jobs command](jobs.md) — List registered jobs and their state.
- [led command](commands.md) — Inspect or control the built-in status LED when available.
- [link command](link.md) — List active SolarOS Link instances and their queue/protocol counters.
- [log command](commands.md) — Show runtime log ring status.
- [ls command](commands.md) — List files. Hidden files are shown only with -a; sizes are human-readable with -h.
- [man command](commands.md) — Read or search the package-aware SolarOS manual.
- [mem command](commands.md) — Print heap status; policy also shows allocation-class counters, guarded fallback limits, and the last tagged failure.
- [meshcore command](commands.md) — Show MeshCore identity, radio, packet, delivery, duplicate, memory, and stack state.
- [messages command](commands.md) — Show bounded-store, persistence, drop, and live provider state.
- [midi command](commands.md) — Show MIDI worker, traffic, parser, and queue status.
- [mkdir command](commands.md) — Create directories.
- [mqtt command](commands.md) — MQTT/MQTTS client.
- [mv command](commands.md) — Rename or move a file or matched set.
- [neopixel command](commands.md) — List attached WS2812/NeoPixel strips.
- [netscan command](commands.md) — Scan TCP ports on one host or a capped IPv4 range.
- [ntp command](commands.md) — Sync the wall clock from NTP.
- [nvs command](commands.md) — Show the default NVS partition size, entry usage, and namespace count.
- [onewire command](commands.md) — Show every registered named 1-Wire bus, or one selected bus.
- [osc command](commands.md) — Inspect named outbound OSC bindings and their live source, value, send, and error state.
- [ota command](commands.md) — Show running and configured OTA state.
- [outbox command](commands.md) — List pending outbound messages. Sent and failed messages remain in conversation history, not Outbox.
- [ping command](commands.md) — Send ICMP echo requests. Without count, ping runs until app-exit.
- [pkg command](commands.md) — Print compiled package groups and build units.
- [pocsag command](commands.md) — Show POCSAG receiver configuration, counters, correction statistics, and RSSI.
- [port command](commands.md) — List byte-stream ports.
- [power command](commands.md) — Inspect and configure power policy.
- [pwm command](commands.md) — Show PWM state.
- [radio command](commands.md) — Open the packet-radio TUI with live status and editable common config.
- [ramfs command](commands.md) — List PSRAM-backed volatile filesystem mounts.
- [reboot command](commands.md) — Restart the board.
- [rm command](commands.md) — Remove files. -f allows directories; -rf removes directories recursively.
- [session command](commands.md) — List display sessions, port shells, and retained port-owned application sessions with their owner.
- [sessions command](commands.md) — List display app sessions, display shell sessions, and port shell sessions.
- [setterm command](commands.md) — Configure terminal/input preferences. Without arguments, opens the display TUI when available.
- [sh command](commands.md) — Run a simple SolarOS shell script from storage.
- [sleep command](commands.md) — Enter explicit light sleep.
- [spi command](commands.md) — Show every named SPI bus, or one selected bus.
- [sshkey command](commands.md) — Show default SSH key status.
- [status command](commands.md) — Print a compact system status summary.
- [stream command](commands.md) — List dynamic typed stream endpoints.
- [suspend command](commands.md) — Turn off the primary display and temporarily use the lowpower profile while services and jobs continue. Press KEY to resume.
- [temperature command](commands.md) — Read the board temperature sensor when available.
- [time command](commands.md) — Show or set the local time.
- [top command](commands.md) — Print FreeRTOS task resource information when available.
- [uart command](commands.md) — Show the default uart0 or a selected named UART bus.
- [unzip command](commands.md) — List or extract a ZIP archive.
- [uptime command](commands.md) — Print elapsed time since boot.
- [version command](commands.md) — Print the SolarOS version and firmware flavor.
- [wait command](commands.md) — Pause the calling shell or shell script for 0 through 86400 seconds.
- [watch command](commands.md) — Repeat another shell command until Esc, q, or the app-exit key is pressed.
- [wifi command](commands.md) — Open the Wi-Fi display TUI when launched from the display shell.
- [wireguard command](commands.md) — Show profile, tunnel, route, peer, DNS, and kill-switch state without printing key material.
- [xfer command](commands.md) — Send or receive files over a byte-stream port.
- [zip command](commands.md) — Create a ZIP archive. -0 stores without compression.

## Applications

- [agent application](apps.md#agent) — Native Responses/Chat-Completions LLM client and SolarOS agent control plane. It streams model text directly to the active shell and exposes typed system-status, storage-listing, job-listing, display-discovery, and optional Python/Lua execution tools. agent tools shows risk, policy, and runtime availability.
- [Agent service and tool reference](agent.service.md) — Provider contract, typed tools, policy, resource bounds, and roadmap
- [aplay application](apps.md#aplay) — Play audio files through the default registered playback endpoint. WAV and MP3 are supported when an output device is present. MP3 decoding is provided by the shared, device-independent audio codec service. aplay prints the source details, plays the file once through the shared background audio player, and then returns to the prompt without clearing existing terminal output. It can be launched from display, UART, USB CDC, Telnet, and other port shells.
- [Application reference](apps.md) — Usage, controls, and examples for every foreground application
- [arecord application](apps.md#arecord) — Record the default registered capture endpoint to a WAV file. This requires a registered input device; it does not require built-in board audio. With -d, recording stops after the specified number of seconds. Without -d, recording continues until app-exit, the storage fills, or the WAV size limit is reached. It can be launched from display, UART, USB CDC, Telnet, and other port shells.
- [calc application](apps.md#calc) — Scientific calculator and function plotter. On a graphical display, calc opens an expression list beside a Cartesian plot. From UART, USB CDC, Telnet, or any other text-only shell, the same command opens a scientific REPL without the plot pane. calc --tui forces that REPL even when graphics are available.
- [chat application](apps.md#chat) — Tabbed provider-neutral conversation client. The Channels tab lists gateway and radio conversations. Enter selects a conversation and opens its bounded shared history on the Chat tab, which also contains the message/command input. The app opens and remains useful offline; network or radio transport jobs connect independently.
- [clock application](apps.md#clock) — Full-screen graphical seven-segment clock, alarm countdown, and stopwatch.
- [com application](apps.md#com) — Serial terminal for a bidirectional byte-stream port. Display-keyboard or port-shell input is forwarded to the selected port, and received bytes are drawn in the active terminal. The port may be a UART or a virtual port such as a peer-bound SolarOS Link stream.
- [contacts application](apps.md#contacts) — Provider-neutral address book for gateway and MeshCore identities. Contacts can carry multiple provider endpoints while retaining trust independently for each endpoint. A signed MeshCore advert creates a discovered endpoint: the signature proves possession of the advertised key, not the human identity behind it.
- [curl application](apps.md#curl) — HTTP client for quick text downloads and diagnostics. It can print response data to the terminal or save it to a file.
- [edit application](apps.md#edit) — Text editor for files on mounted storage. It supports cursor navigation, selection, clipboard operations, text-size changes, and syntax highlighting for known source files. The editor supports files up to 256 KiB on boards with PSRAM and 32 KiB on boards without PSRAM. Use hexedit for binary files.
- [email application](apps.md#email) — Receive-only IMAPS client for the configured mailbox. The app shows the provider-specific message list while every newly synchronized message is also published to the universal inbox and its shared status-bar unread counter.
- [files application](apps.md#files) — File manager inspired by Midnight Commander. Its normal mode provides two panes for copy, move, delete, and launch workflows on mounted storage. Launcher mode provides a minimal single-pane application menu suitable for a startup script.
- [Flash another ESP board](flash.md) — Download verified SolarOS factory images and program another ESP board over UART
- [flash application](apps.md#flash) — Download verified SolarOS factory artifacts to SD and program another supported ESP board over UART. The browser refreshes the signed catalog on request and shows which board, flavor, and version artifacts are already cached. Its tree starts folded and retains its selection and fold state after operations. Delete removes a selected cached artifact after confirmation. The shell form accepts a named UART plus optional boot and reset GPIO pins.
- [funcgen application](apps.md#funcgen) — Audio-only function generator built on the shared real-time Synth service. It emits signed 16-bit stereo PCM and can use the default playback device or an explicitly selected runtime playback stream, including an attached LEDC PWM audio expansion.
- [gameboy application](apps.md#gameboy) — Experimental original Game Boy (DMG) emulator for the Waveshare ESP32-S3-RLCD-4.2 and the full-PAL Freenove ESP32-WROVER v3.0 target. It is included in the retro and rover-retro flavors. The application loads a user-supplied ROM into PSRAM, renders its four shades as a 320x288 dithered image, and writes battery-backed cartridge RAM beside the ROM as a .sav file. Game Boy Color-only ROMs and ROMs larger than 4 MiB are rejected.
- [help application](apps.md#help) — Foreground browser for the package-aware SolarOS manual. The foldable tree groups the topics compiled for the current firmware and shows whether it is using the embedded copy or a verified downloaded revision. All groups start folded. The selection, scroll position, and fold state remain unchanged after a topic closes.
- [hexedit application](apps.md#hexedit) — Two-pane binary editor for files on mounted storage. Each row shows a file offset, hexadecimal bytes, and their synchronized printable ASCII view. The number of bytes per row adapts to the terminal width. It uses the same 256 KiB PSRAM and 32 KiB internal-memory limits as edit.
- [inbox application](apps.md#inbox) — Universal incoming-message browser for pages, chat notifications, mail, and other background producers. It reads the same shared inbox that supplies the status-bar unread count. Messages and read state survive reboot in the bounded /.inbox/messages.bin store; the service retains at most 64 entries and keeps the file below 32 KB even when internal flash is the only storage.
- [invaders application](apps.md#invaders) — Graphical arcade shooter.
- [io application](apps.md#io) — Interactive expansion I/O manager. Its default Layout view presents the board's connectors in their physical arrangement, followed by the existing pin, named-bus, and resource-claim views. It uses the same ownership and validation services as the gpio, i2c, spi, uart, midi, onewire, and expansion commands.
- [less application](apps.md#less) — Terminal pager for text files. It preserves original text layout and is useful for quick file inspection.
- [logic application](apps.md#logic) — On-device logic analyzer waveform viewer. It displays the latest capture made by the shared logic analyzer service or the SUMP job. With pin arguments it makes a new local capture before opening the viewer.
- [lua application](apps.md#lua) — Embedded Lua runtime. It can run an interactive REPL or execute .lua scripts from storage. Lua scripts can use SolarOS service bindings when the selected firmware includes the corresponding packages. Foreground scripts can consume touch coordinates, relative mouse motion, buttons, and joystick axes through solaros.input.
- [Native SolarOS agent](agent.md) — Configure and use the resumable LLM agent and its typed tools
- [notes application](apps.md#notes) — Markdown-backed checklist and category manager. It stores unchecked and checked items and supports one level of category folding. A persistent bottom help bar shows the available controls, with status or text input directly above it.
- [player application](apps.md#player) — Interactive WAV/MP3 player and the user-facing counterpart to aplay. player keeps a persistent playlist under .player on the current storage root. Opening an audio file from Files adds it to that playlist, selects it, and starts playback. Missing files remain listed so removable media can be reattached.
- [Playground](playground.md) — Browse, install, uninstall, and run community Python and Lua applications
- [playground application](apps.md#playground) — Browse the configured community catalog as a foldable category tree, search applications, and install, update, uninstall, or run Python and Lua scripts.
- [plot application](apps.md#plot) — Graphical plotter for DAQ CSV files and live scalar streams. It is compatible with CSV generated by the daq job.
- [python application](apps.md#python) — Embedded MicroPython runtime. It can run an interactive REPL, .py scripts, or .mpy files from storage. Python scripts can use SolarOS service bindings when the selected firmware includes the corresponding packages. Foreground scripts can consume touch coordinates, relative mouse motion, buttons, and joystick axes through solaros.input.
- [reader application](apps.md#reader) — Graphical document reader for plain text, Markdown, and EPUB. It remembers reading position and zoom per opened file when storage is available.
- [recorder application](apps.md#recorder) — Interactive GUI/TUI counterpart to arecord. Recorder writes PCM WAV files so the channel count, sample rate, and resolution travel with the recording and the result can be played immediately. It accepts any registered signed-16-bit PCM capture stream. On the Waveshare board it initially selects audio0.capture. Mono/stereo output, 8/16-bit file resolution, and sample rates from 8 kHz through 48 kHz are converted from the selected stream as necessary.
- [scp application](apps.md#scp) — SCP file transfer over SSH. It supports password or key authentication through the shared SSH transport and host lookup/known-host storage. When user@ is omitted, SCP uses the NVS-backed SolarOS identity user.
- [sheet application](apps.md#sheet) — CSV viewer for small data tables. It is intended as a companion to daq logs and simple spreadsheet-like inspection.
- [ssh application](apps.md#ssh) — Interactive SSH client. It supports password and key authentication, known hosts, hostname lookup through /.ssh/hosts, UTF-8 text, VT-style controls, and remote full-screen terminal applications. When user@ is omitted, SSH uses the NVS-backed SolarOS identity user.
- [synth application](apps.md#synth) — Open the native synthesizer and sound designer:
- [telnet application](apps.md#telnet) — Telnet client for classic TCP terminal sessions. It supports basic Telnet option negotiation, terminal type reporting, window size reporting, and raw mode.
- [view application](apps.md#view) — Graphical image viewer. It supports the image formats compiled into the current firmware, including common PNG/JPEG/GIF/WebP paths and automatic animated GIF playback when the media package is enabled.
- [web application](apps.md#web) — Simple graphical web browser for lightweight HTML pages. It shares document and image rendering infrastructure with reader where possible.
- [webradio application](apps.md#webradio) — Stream a direct MP3 URL through the default registered audio output. On a graphical display shell, WebRadio opens a two-tab media-player GUI. On UART, USB CDC, Telnet, SSH, and other text shells, it opens a station-list TUI.
- [writer application](apps.md#writer) — Resumable graphical Markdown editor for PSRAM display boards. Inactive blocks are formatted like reader; the block containing the cursor and every block touched by a selection show their exact Markdown source. edit remains the portable text editor for port shells and boards without graphics or PSRAM.

## Background jobs

- [Background job reference](jobs.reference.md) — Configuration, ownership, and examples for every background job
- [Background jobs](jobs.md) — Inspect and control bounded background workers
- [batmon job](jobs.reference.md#batmon) — Battery monitor. It periodically samples battery voltage, maintains a smoothed trend, estimates power state, and can request light sleep when the configured minimum voltage is reached.
- [bridge job](jobs.reference.md#bridge) — Bidirectional byte bridge between two byte-stream ports, or between one byte-stream port and an active SolarOS Link instance.
- [chatd job](jobs.reference.md#chatd) — Local SolarOS chat gateway server. It is useful for testing the chat app or for small trusted local networks.
- [controls job](jobs.reference.md#controls) — Continuous-control mapper. It samples every configured scalar-stream control at 50 Hz, applies smoothing, deadband, calibration, and inversion, then updates changed native parameter and MIDI CC bindings.
- [daq job](jobs.reference.md#daq) — Data acquisition job. It captures scalar and event streams to timestamped CSV, or one byte or PCM audio source directly to a raw file.
- [displayd job](jobs.reference.md#displayd) — Authenticated HTTP display and remote control. It has two modes:
- [email-sync job](jobs.reference.md#email-sync) — Receive-only IMAPS mailbox polling job. It fetches mail into the provider-local email app and publishes each new message to the universal inbox.
- [espnow-link job](jobs.reference.md#espnow-link) — ESP-NOW adapter for the transport-independent SolarOS Link service.
- [gateway-sync job](jobs.reference.md#gateway-sync) — Background synchronizer for the gateway messaging provider. Start and stop it explicitly, using the same lifecycle as email-sync:
- [gpio-keys job](jobs.reference.md#gpio-keys) — Maps runtime-safe GPIO inputs to SolarOS keyboard presses. The job configures each pin as an input with its internal pull-up enabled, treats a low level as pressed, and applies the same 25 ms debounce used by fixed board buttons. Each debounced transition publishes a generic SolarOS key press or release. Held keys use the system repeat rate configured by setterm keyrate.
- [httpd job](jobs.reference.md#httpd) — Static HTTP file server for a folder on mounted storage.
- [log job](jobs.reference.md#log) — Runtime SolarOS log follower. It mirrors log entries to a byte-stream port or appends them to a file.
- [meshcore job](jobs.reference.md#meshcore) — Non-forwarding MeshCore companion provider for Contacts and Messages.
- [midi job](jobs.reference.md#midi) — Bidirectional MIDI transport on an exclusive named MIDI bus. The bus selects an available UART controller internally; users supply only its MIDI name, TX and RX pins, and an optional baud rate.
- [ntp-sync job](jobs.reference.md#ntp-sync) — Network time synchronization job. It updates the SolarOS wall clock from NTP and also updates the hardware RTC when the board provides one.
- [osc job](jobs.reference.md#osc) — OSC 1.0 IPv4 UDP adapter for automatic incoming native-parameter writes and explicit named outbound stream, event-stream, or normalized-control bindings.
- [pocsag job](jobs.reference.md#pocsag) — POCSAG pager receiver job. It configures a registered packet radio for a continuous POCSAG byte stream, frames successive 64-byte batches, filters pages to one receiver identity code (RIC), decodes alphanumeric or numeric payloads, and publishes completed messages to the universal inbox.
- [ps2-keyboard job](jobs.reference.md#ps2-keyboard) — Receives keyboard scan-code set 2 from an exclusive named PS/2 bus and publishes press and release transitions through the generic SolarOS input service. This job is a compatibility wrapper around a ps2-keyboard expansion attachment; new configurations can attach the device directly.
- [radio-link job](jobs.reference.md#radio-link) — Packet-radio adapter for the transport-independent SolarOS Link service.
- [slip job](jobs.reference.md#slip) — IPv4 SLIP gateway on a byte-stream port. This is intended for retro machines, headless boards, and serial networking experiments.
- [sump job](jobs.reference.md#sump) — SUMP-compatible logic analyzer server on cdc0. It claims the CDC port and uses the shared logic analyzer service for acquisition. PulseView and sigrok can connect with the OpenBench Logic Sniffer/SUMP serial driver.
- [telnetd job](jobs.reference.md#telnetd) — Remote Telnet shell server. The listener is a background job; each accepted connection is attached to its own normal SolarOS port-shell session.

## Networking and security

- [Open Sound Control](osc.md) — Control live app parameters and publish named stream or control bindings over OSC 1.0 UDP
- [SSH identity keys](ssh_keys.md) — Inspect, share, generate, and remove the default SSH key pair
- [Wi-Fi, WireGuard, MQTT, and network APIs](network.md) — Connect, inspect, and communicate over installed network services

## Hardware and expansion

- [Audio, input, and clipboard APIs](media.input.md) — Use installed media and generic input services
- [Continuous controls](controls.md) — Map analog and other scalar inputs to app parameters or MIDI CC
- [Expansion drivers and attached devices](expansion.md) — Discover, attach, and detach package-gated expansion devices
- [Expansion hardware reference](expansion.reference.md) — Resource rules, workflows, drivers, bindings, and wiring examples
- [GPIO, ADC, PWM, and LED APIs](gpio.analog.md) — Use runtime-safe digital and analog expansion pins
- [Time, battery, and environment APIs](time.sensors.md) — Read clocks, battery state, temperature, and humidity

## Scripting APIs

- [Compatibility I/O modules](compatibility.io.md) — Use the legacy single-bus I2C, SPI, UART, and OneWire APIs
- [Digital signal processing](dsp.md) — Portable fixed-point DSP operations, streaming contexts, and script APIs
- [Lua API reference](lua.md) — Complete Lua service API, conventions, and examples
- [Lua graphics API](lua.gfx.md) — Draw through SolarOS displays from Lua
- [Lua text user-interface API](lua.tui.md) — Build terminal applications from Lua
- [Named runtime buses](buses.md) — Create and use resource-owned I2C, SPI, UART, MIDI, OneWire, and PS/2 buses
- [Python API reference](python.md) — Complete MicroPython service API, conventions, and examples
- [Python graphics API](python.gfx.md) — Draw through SolarOS displays from MicroPython
- [Python text user-interface API](python.tui.md) — Build terminal applications from MicroPython

## System services

- [Device identity](identity.md) — Read and configure the NVS-backed user and hostname
- [Foreground sessions and applications](sessions.apps.md) — Create shells and inspect resumable foreground applications
- [MeshCore companion messaging](meshcore.md) — Secure direct and shared-group messages over a claimed packet radio
- [Messaging, contacts, and credential security](messaging.md) — Provider-neutral messaging identities, trust, persistence, and secret handling
- [SolarOS Link](link.md) — Packet messaging and reliable virtual serial ports over packet radio or ESP-NOW

## Boards and firmware

- [Boards and hardware targets](boards.md) — Supported boards, capabilities, porting structure, and validation
- [Firmware packages and flavors](packages.md) — Understand package ownership, groups, flavors, and custom builds

The TOML frontmatter on each topic controls package availability, search metadata, and placement in the documentation tree. Edit the topic itself; do not maintain a separate device or website copy.
