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
- [audio command](commands.md) — Show audio service state and global speaker level.
- [battery command](commands.md) — Show voltage, estimated charge, power source, config, and monitor trend.
- [ble command](commands.md) — Show BLE keyboard state.
- [board command](commands.md) — Print board ID, name, and capabilities.
- [cat command](commands.md) — Print a small text file.
- [cd command](commands.md) — Change current shell directory.
- [clear command](commands.md) — Clear the active shell terminal.
- [close command](commands.md) — Close an app or shell session while preserving the final interactive shell.
- [commands command](commands.md) — List built-in shell commands.
- [cp command](commands.md) — Copy a file or matched set.
- [daq command](commands.md) — Print DAQ usage.
- [date command](commands.md) — Show or set the local date.
- [df command](commands.md) — Show free space on mounted storage volumes.
- [display command](commands.md) — List drawable display targets, draw a test pattern, or change driver-specific display settings.
- [dpad command](commands.md) — Show ADC D-pad pins, raw values, zones, and calibration thresholds.
- [email command](commands.md) — Open the receive-only email app.
- [engine command](commands.md) — Print or reset generic engine utilization counters for CPU/SIMD-style backends and vector bulk operations.
- [exit command](commands.md) — Close the current UART, USB CDC, or telnet shell. The display shell remains active.
- [expansion command](expansion.md) — Show expansion capabilities, named buses and leases, connector resources, active devices, and resource claims.
- [fg command](commands.md) — Resume a display app or display shell session.
- [gpio command](commands.md) — List board GPIOs with free, releasable, or fixed pin policy.
- [help command](help.md) — Browse the package-aware manual or manage its signed exact-version SD copy.
- [humidity command](commands.md) — Read the board humidity sensor when available.
- [i2c command](commands.md) — Show every named I2C bus, or one selected bus.
- [identity command](identity.md) — Show the configured user and hostname.
- [inbox command](commands.md) — Open the universal incoming-message browser.
- [job command](jobs.md) — Show one job or all jobs.
- [jobs command](jobs.md) — List registered jobs and their state.
- [joystick command](commands.md) — Show joystick axes, raw values, direction, and thresholds.
- [led command](commands.md) — Inspect or control the built-in status LED when available.
- [log command](commands.md) — Show runtime log ring status.
- [link command](link.md) — Send and receive transport-independent SolarOS Link messages.
- [ls command](commands.md) — List files. Hidden files are shown only with -a; sizes are human-readable with -h.
- [man command](commands.md) — Read or search the package-aware SolarOS manual.
- [mem command](commands.md) — Print heap status; policy also shows allocation-class counters, guarded fallback limits, and the last tagged failure.
- [mkdir command](commands.md) — Create directories.
- [mqtt command](commands.md) — MQTT/MQTTS client.
- [mv command](commands.md) — Rename or move a file or matched set.
- [netscan command](commands.md) — Scan TCP ports on one host or a capped IPv4 range.
- [ntp command](commands.md) — Sync the wall clock from NTP.
- [onewire command](commands.md) — Show every registered named 1-Wire bus, or one selected bus.
- [ota command](commands.md) — Show running and configured OTA state.
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
- [sd command](commands.md) — Show SD/storage status.
- [session command](commands.md) — List display app sessions, display shell sessions, and port shell sessions.
- [sessions command](commands.md) — List display app sessions, display shell sessions, and port shell sessions.
- [setterm command](commands.md) — Configure terminal/input preferences. Without arguments, opens the display TUI when available.
- [sh command](commands.md) — Run a simple SolarOS shell script from storage.
- [sleep command](commands.md) — Enter explicit light sleep.
- [spi command](commands.md) — Show every named SPI bus, or one selected bus.
- [sshkey command](commands.md) — Show default SSH key status.
- [status command](commands.md) — Print a compact system status summary.
- [stream command](commands.md) — List timestamped data streams.
- [temperature command](commands.md) — Read the board temperature sensor when available.
- [time command](commands.md) — Show or set the local time.
- [top command](commands.md) — Print FreeRTOS task resource information when available.
- [uart command](commands.md) — Show the default uart0 or a selected named UART bus.
- [unzip command](commands.md) — List or extract a ZIP archive.
- [uptime command](commands.md) — Print elapsed time since boot.
- [version command](commands.md) — Print the SolarOS version and firmware flavor.
- [watch command](commands.md) — Repeat another shell command until Esc, q, or the app-exit key is pressed.
- [wifi command](commands.md) — Open the Wi-Fi display TUI when launched from the display shell.
- [xfer command](commands.md) — Send or receive files over a byte-stream port.
- [zip command](commands.md) — Create a ZIP archive. -0 stores without compression.

## Applications

- [agent application](apps.md#agent) — Native Responses/Chat-Completions LLM client and SolarOS agent control plane. It streams model text directly to the active shell and exposes typed system-status, storage-listing, job-listing, display-discovery, and optional Python/Lua execution tools. agent tools shows risk, policy, and runtime availability.
- [Agent service and tool reference](agent.service.md) — Provider contract, typed tools, policy, resource bounds, and roadmap
- [aplay application](apps.md#aplay) — Play audio files through the board audio output. WAV and MP3 are supported when the audio package is compiled and the board has audio hardware.
- [Application reference](apps.md) — Usage, controls, and examples for every foreground application
- [arecord application](apps.md#arecord) — Record microphone input to a WAV file. This requires the audio package and board microphone hardware.
- [calc application](apps.md#calc) — Scientific calculator with a graphical expression worksheet and function plotter on displays, plus the same bounded expression engine in a text REPL and one-shot shell mode.
- [chat application](apps.md#chat) — Two-pane chat client. The left pane lists channels, the right pane shows conversation history, and the bottom line is the message/command input.
- [clock application](apps.md#clock) — Full-screen graphical seven-segment clock, alarm countdown, and stopwatch.
- [com application](apps.md#com) — Serial terminal for named UART buses, with optional hardware autobaud detection and an offset/hex/ASCII receive view. It works from display and port shells.
- [curl application](apps.md#curl) — HTTP client for quick text downloads and diagnostics. It can print response data to the terminal or save it to a file.
- [edit application](apps.md#edit) — Text editor for files on mounted storage. It supports cursor navigation, selection, clipboard operations, text-size changes, and syntax highlighting for known source files. The editor supports files up to 256 KiB on boards with PSRAM and 32 KiB on boards without PSRAM.
- [email application](apps.md#email) — Receive-only IMAPS client for the configured mailbox. The app shows the provider-specific message list while every newly synchronized message is also published to the universal inbox and its shared status-bar unread counter.
- [files application](apps.md#files) — Two-pane file manager inspired by Midnight Commander. It is intended for quick copy, move, delete, and launch workflows on mounted storage.
- [help application](apps.md#help) — Foreground browser for the package-aware SolarOS manual. The foldable tree groups the topics compiled for the current firmware and shows whether it is using the embedded copy or a verified downloaded revision.
- [inbox application](apps.md#inbox) — Universal incoming-message browser for pages, chat notifications, mail, and other background producers. It reads the same shared inbox that supplies the status-bar unread count. Messages and read state survive reboot in the bounded /.inbox/messages.bin store; the service retains at most 64 entries and keeps the file below 32 KB even when internal flash is the only storage.
- [invaders application](apps.md#invaders) — Graphical arcade shooter.
- [io application](apps.md#io) — Interactive expansion I/O manager. It presents the board's expansion pins, named buses, and resource claims in one TUI and uses the same ownership and validation services as the gpio, i2c, spi, uart, onewire, and expansion commands.
- [less application](apps.md#less) — Terminal pager for text files. It preserves original text layout and is useful for quick file inspection.
- [logic application](apps.md#logic) — On-device logic analyzer waveform viewer. It displays the latest capture made by the shared logic analyzer service or the SUMP job. With pin arguments it makes a new local capture before opening the viewer.
- [lua application](apps.md#lua) — Embedded Lua runtime. It can run an interactive REPL or execute .lua scripts from storage. Lua scripts can use SolarOS service bindings when the selected firmware includes the corresponding packages.
- [Native SolarOS agent](agent.md) — Configure and use the resumable LLM agent and its typed tools
- [notes application](apps.md#notes) — Markdown-backed checklist and category manager. It stores unchecked and checked items and supports one level of category folding.
- [Playground](playground.md) — Browse, install, uninstall, and run community Python and Lua applications
- [playground application](apps.md#playground) — Browse the configured community catalog as a foldable category tree, search applications, and install, update, uninstall, or run Python and Lua scripts.
- [plot application](apps.md#plot) — Graphical plotter for DAQ CSV files and live scalar streams. It is compatible with CSV generated by the daq job.
- [python application](apps.md#python) — Embedded MicroPython runtime. It can run an interactive REPL, .py scripts, or .mpy files from storage. Python scripts can use SolarOS service bindings when the selected firmware includes the corresponding packages.
- [reader application](apps.md#reader) — Graphical document reader for plain text, Markdown, and EPUB. It remembers reading position and zoom per opened file when storage is available.
- [scp application](apps.md#scp) — SCP file transfer over SSH. It supports password or key authentication through the shared SSH transport and host lookup/known-host storage. When user@ is omitted, SCP uses the NVS-backed SolarOS identity user.
- [sheet application](apps.md#sheet) — CSV viewer for small data tables. It is intended as a companion to daq logs and simple spreadsheet-like inspection.
- [ssh application](apps.md#ssh) — Interactive SSH client. It supports password and key authentication, known hosts, hostname lookup through /.ssh/hosts, UTF-8 text, VT-style controls, and remote full-screen terminal applications. When user@ is omitted, SSH uses the NVS-backed SolarOS identity user.
- [telnet application](apps.md#telnet) — Telnet client for classic TCP terminal sessions. It supports basic Telnet option negotiation, terminal type reporting, window size reporting, and raw mode.
- [view application](apps.md#view) — Graphical image viewer. It supports the image formats compiled into the current firmware, including common PNG/JPEG/GIF/WebP paths and automatic animated GIF playback when the media package is enabled.
- [web application](apps.md#web) — Simple graphical web browser for lightweight HTML pages. It shares document and image rendering infrastructure with reader where possible.
- [writer application](apps.md#writer) — Resumable hybrid WYSIWYG Markdown editor for graphical PSRAM boards. The active block shows exact Markdown source while inactive blocks remain formatted.

## Background jobs

- [Background job reference](jobs.reference.md) — Configuration, ownership, and examples for every background job
- [Background jobs](jobs.md) — Inspect and control bounded background workers
- [batmon job](jobs.reference.md#batmon) — Battery monitor. It periodically samples battery voltage, maintains a smoothed trend, estimates power state, and can request light sleep when the configured minimum voltage is reached.
- [bridge job](jobs.reference.md#bridge) — Bidirectional byte bridge between two ports or a port and SolarOS Link.
- [chat-sync job](jobs.reference.md#chat-sync) — Background client synchronizer for the transport-neutral chat service. Start and stop it explicitly, using the same lifecycle as email-sync:
- [chatd job](jobs.reference.md#chatd) — Local SolarOS chat gateway server. It is useful for testing the chat app or for small trusted local networks.
- [daq job](jobs.reference.md#daq) — Data acquisition job. It captures scalar streams to timestamped CSV, or one byte stream directly to a raw file.
- [displayd job](jobs.reference.md#displayd) — Authenticated HTTP display and remote control. It has two modes:
- [email-sync job](jobs.reference.md#email-sync) — Receive-only IMAPS mailbox polling job. It fetches mail into the provider-local email app and publishes each new message to the universal inbox.
- [httpd job](jobs.reference.md#httpd) — Static HTTP file server for a folder on mounted storage.
- [log job](jobs.reference.md#log) — Runtime SolarOS log follower. It mirrors log entries to a byte-stream port or appends them to a file.
- [meshcore job](jobs.reference.md#meshcore) — Non-forwarding MeshCore companion messaging over a claimed packet radio.
- [ntp-sync job](jobs.reference.md#ntp-sync) — Network time synchronization job. It updates the SolarOS wall clock from NTP and also updates the hardware RTC when the board provides one.
- [pocsag job](jobs.reference.md#pocsag) — POCSAG pager receiver job. It configures a registered packet radio for a continuous POCSAG byte stream, frames successive 64-byte batches, filters pages to one receiver identity code (RIC), decodes alphanumeric or numeric payloads, and publishes completed messages to the universal inbox.
- [radio-link job](jobs.reference.md#radio-link) — Packet-radio adapter for transport-independent SolarOS Link messages.
- [slip job](jobs.reference.md#slip) — IPv4 SLIP gateway on a byte-stream port. This is intended for retro machines, headless boards, and serial networking experiments.
- [sump job](jobs.reference.md#sump) — SUMP-compatible logic analyzer server on cdc0. It claims the CDC port and uses the shared logic analyzer service for acquisition. PulseView and sigrok can connect with the OpenBench Logic Sniffer/SUMP serial driver.
- [telnetd job](jobs.reference.md#telnetd) — Remote Telnet shell server. The listener is a background job; each accepted connection is attached to its own normal SolarOS port-shell session.

## Networking and security

- [SSH identity keys](ssh_keys.md) — Inspect, generate, and remove the default SSH key pair
- [Wi-Fi, MQTT, and network APIs](network.md) — Connect, inspect, and communicate over installed network services

## Hardware and expansion

- [Audio, BLE keyboard, and clipboard APIs](media.input.md) — Use installed media and input services
- [Expansion drivers and attached devices](expansion.md) — Discover, attach, and detach package-gated expansion devices
- [Expansion hardware reference](expansion.reference.md) — Resource rules, workflows, drivers, bindings, and wiring examples
- [GPIO, ADC, PWM, and LED APIs](gpio.analog.md) — Use runtime-safe digital and analog expansion pins
- [Time, battery, and environment APIs](time.sensors.md) — Read clocks, battery state, temperature, and humidity

## Scripting APIs

- [Compatibility I/O modules](compatibility.io.md) — Use the legacy single-bus I2C, SPI, UART, and OneWire APIs
- [Lua API reference](lua.md) — Complete Lua service API, conventions, and examples
- [Lua graphics API](lua.gfx.md) — Draw through SolarOS displays from Lua
- [Lua text user-interface API](lua.tui.md) — Build terminal applications from Lua
- [Named runtime buses](buses.md) — Create and use resource-owned I2C, SPI, UART, and OneWire buses
- [Python API reference](python.md) — Complete MicroPython service API, conventions, and examples
- [Python graphics API](python.gfx.md) — Draw through SolarOS displays from MicroPython
- [Python text user-interface API](python.tui.md) — Build terminal applications from MicroPython

## System services

- [Device identity](identity.md) — Read and configure the NVS-backed user and hostname
- [Foreground sessions and applications](sessions.apps.md) — Create shells and inspect resumable foreground applications
- [MeshCore companion messaging](meshcore.md) — Secure direct and shared-group messages over a claimed packet radio
- [SolarOS Link](link.md) — Transport-independent packet messaging and the radio-link adapter

## Boards and firmware

- [Boards and hardware targets](boards.md) — Supported boards, capabilities, porting structure, and validation
- [Firmware packages and flavors](packages.md) — Understand package ownership, groups, flavors, and custom builds

The TOML frontmatter on each topic controls package availability, search metadata, and placement in the documentation tree. Edit the topic itself; do not maintain a separate device or website copy.
