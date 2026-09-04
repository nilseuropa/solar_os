# SolarOS Changelog

## 4.x

- **4.10.18** — 2026-09-04 — `help update` now accepts signed manuals that
  omit package-gated topics from the active runtime index, fixing verification
  failures on boards without the ADC D-pad service.
- **4.10.17** — 2026-09-04 — Shell path completion now understands quoted and
  backslash-escaped input and quotes completed filenames that contain spaces.
- **4.10.16** — 2026-09-04 — `ble forget` now clears the remembered
  keyboard's cached GATT service database as well as its bond, forcing service
  rediscovery when keyboard firmware changes move attribute handles.
- **4.10.15** — 2026-09-04 — Added the CL-32 target with its 384x168 ST7305
  reflective display, ATmega808-backed keyboard and battery monitor, PWM
  buzzer audio, SDSPI storage, PCF85063 RTC, native USB CDC, and expansion
  buses. Extended the ST7305 driver for the smaller panel geometry and board
  rotation. Diagnostic tones now follow the selected default playback device,
  including runtime-attached outputs. Edit gained `F2` Save, `F3` Find, and
  `F10` Quit aliases while retaining its portable Ctrl and Esc controls. The
  Waveshare ESP32-S3-RLCD-4.2 target is now named SolarTerm, with the
  `solar_term` board and PlatformIO environment identifiers. Inactive optional
  expansion-driver registries and the Telnet listener stack now use PSRAM,
  restoring internal RAM without moving scheduler, session, transport, or
  active hardware state out of internal memory.
- **4.10.14** — 2026-09-03 — Added optional one-hop SolarOS Link repeating to
  the packet-radio `radio-link` job. A headless device with one compatible
  radio can retransmit text, binary, acknowledgement, broadcast, and virtual
  stream frames while preserving the original Link identities. Relayed-frame
  marking, randomized forwarding delay, duplicate suppression, and a bounded
  queue prevent forwarding loops and reduce collisions. Job status reports
  forwarded, suppressed, queued, dropped, and invalid-frame counters.
- **4.10.13** — 2026-09-03 — Added an IPv4 layer-2 Wi-Fi repeater. The
  `wifi repeater` command and matching Python/Lua APIs bridge DHCP and IPv4/ARP
  traffic between an upstream station and downstream SoftAP without NAT, so
  downstream clients remain on the upstream subnet. Status includes learned
  clients and forwarding counters. The SoftAP automatically repeats the active
  saved profile's SSID and password; stopping retains the upstream link.
- **4.10.12** — 2026-09-02 — Added generic RTC alarm and timer support,
  including PCF85063 hardware alarms, plus a portable persistent scheduler with
  shell and Python/Lua APIs. Added NVS inventory and selective erase commands.
  Hardened downloadable help with signed metadata, verified cached pages, a
  compact catalog, larger page limits, runtime package gating, and corrected
  focused command pages and aliases.
- **4.10.11** — 2026-09-02 — Standardized the built-in flavor set around the
  board-first Applications, Background jobs, and Drivers model. Added the
  dedicated VGA32 flavor and aligned default board mappings, package
  resolution, documentation, and flash partition layouts.
- **4.10.10** — 2026-09-01 — Kept Telnet sessions responsive with a scoped
  low-latency Wi-Fi lease that restores the normal power-saving policy after
  disconnect. Added the PCM1808 capture-only I2S expansion driver and matching
  recording and Python/Lua support. Added interactive flavor configuration and
  the SolarOS builder for selecting packages and producing board images.
- **4.10.9** — 2026-08-31 — Added trusted MeshCore virtual serial ports. Two
  SolarOS devices can create matching peer-bound streams that carry reliable
  SolarOS Link framing inside end-to-end encrypted MeshCore direct packets, so
  standard MeshCore repeaters can route interactive shell or serial-bridge
  traffic without understanding SolarOS. The new `meshcore stream` commands
  create, inspect, and remove normal byte-stream ports for use with `com`,
  shell sessions, and `bridge`.
- **4.10.8** — 2026-08-31 — Added an unencrypted, anonymous-by-default `ftpd`
  background job with an export-root boundary and optional username/password
  login. Added a resumable two-pane `ftp` application for recursive copy, move,
  delete, directory creation, and viewing between local storage and passive-mode
  FTP servers. Added the shared native FTP client service and matching
  `solaros.ftp` Python and Lua bindings. FTP copy and move operations show
  Files-style progress, and same-directory refreshes preserve both pane
  positions.
- **4.10.7** — 2026-08-31 — Unified foreground application ownership and exit
  handling across native applications, scripts, remote sessions, and port
  shells, making terminal and display handoff deterministic. Game Boy now
  composes a clean frame when its session resumes instead of exposing stale
  display content.
- **4.10.6** — 2026-08-30 — Added a shared raster-frame presenter for bounded
  animation: MONO1, INDEX2, and INDEX8 frame contracts now describe source and
  destination geometry, display targets publish their supported formats and
  preferred 25-30 Hz cadence, and pending frames use latest-wins replacement.
  Game Boy moved into the `games` group and now runs its emulator at the fixed
  native frequency in a dedicated worker while Peanut-GB's 30 Hz LCD frame
  skip keeps unused video work out of the emulation path. It submits compact
  160x144 INDEX2 frames; TFTs render those four colors directly, while RLCD,
  PAL, and VGA use the common ordered-dither MONO1 fallback. Per-target pixel
  budgets select a sustainable scaled raster before presentation. Game Boy
  audio has bounded local gain and reports output blocks and peak level with
  the timing statistics. The obsolete Retro and Rover Retro flavors were
  removed, and the focused `rover-gameboy` flavor omits Invaders to fit classic
  4 MB targets.
  A `streaming_display` capability keeps Game Boy off e-paper targets. Normal
  terminal rendering now redraws changed rows and UI bands only. TFT terminal,
  INDEX8 GUI, and INDEX2 game output share one queued two-line DMA pump and
  upload coalesced changed regions; repeated RLCD direct frames update only the
  centered raster window after the initial full synchronization.
- **4.10.5** — 2026-08-30 — Writer no longer draws inline-code boxes around
  raw Markdown source while editing the active block, improving text
  readability without changing formatted inline-code or fenced-code rendering.
- **4.10.4** — 2026-08-30 — Web gained a visible Back, Reload, and Forward
  toolbar, bidirectional history with restored reading positions, direct
  pointer or touch activation, and non-wrapping link selection. Links now use
  the same font size as surrounding text, and page text locally reflows across
  five Reader-style zoom levels without refetching images.
- **4.10.3** — 2026-08-30 — Added dynamic `ssh` and `scp` host completion from
  aliases in `/.ssh/hosts`, including preservation of an explicit `user@`
  prefix. Unique SCP host matches append the remote-path colon. Edit and
  Hexedit now save with `Ctrl+S`, while `Ctrl+Q` and Esc quit without saving.
  Edit, Less, Reader, Writer, and Web now share `Ctrl+F` Find and `F3` Find
  Next controls with case-insensitive, wrapping search; Less and Reader retain
  their `/`, `n`, and `N` aliases.
- **4.10.2** — 2026-08-30 — Added adaptive color graphics for ILI9341 and
  ST7796 targets. GUI apps negotiate a lazily allocated 8-bit indexed canvas
  when they enter graphics mode and release it when graphics mode ends. TFT
  presentation converts only changed tiles through a small DMA scanline and
  uses compact tile fingerprints instead of a second color framebuffer.
  Missing PSRAM falls back to the original monochrome renderer without
  preventing app launch; one-bit displays retain the existing framebuffer and
  ordered-dither path. Python and Lua graphics gained `rgb(red, green, blue)`.
  Sketch now uses a white, red, blue, and black palette, saves interoperable
  indexed-color PNG files, and imports PNG, JPEG, and GIF images in color.
  View and Web preserve decoded image colors on indexed-color displays while
  retaining their original grayscale buffers on one-bit displays. View now
  scales fit-mode JPEG output during color conversion, avoiding the full RGB
  allocation peak for large photographs. Semantic GUI colors now use the
  persistent `setterm foreground` and `background` colors on color displays;
  explicit image, canvas, and script RGB colors remain literal.
  Display-targeted absolute pointer coordinates now follow that display's
  `setterm orientation` setting.
- **4.10.1** — 2026-08-30 — Added Sketch, the first native pointer-driven GUI
  application. Its Paint-style layout provides Save/Open/Import commands, a
  aligned top menu and sidebar buttons on one equal-sized grid, with tools for
  pen, line, rectangle, ellipse, bucket fill, eraser,
  clear, and stroke weight, plus four-shade color and pattern selectors. Its
  canvas is converted to a single opaque XBM blit for responsive redraws, and
  absolute-pointer motion does not redraw until it changes the image. Line,
  rectangle, and ellipse drags show the exact patterned, weighted result before
  release. Sketch
  launches without a pointer
  capability gate, warns when no pointer is currently registered, and accepts
  pointers attached later at runtime. It saves interoperable grayscale PNG
  files transactionally and imports PNG, JPEG, and GIF images. App state,
  canvas, browser, and decode buffers are cold allocated and released on exit.
- **4.10.0** — 2026-08-29 — Added Synth hold mode, which toggles notes on key
  press for sustained chords without requiring keys to remain held. Improved
  the board configurator handoff by printing exact build and upload commands
  while preserving the selected `SOLAR_OS_BOARD` profile.
- **4.9.2** — 2026-08-29 — Unified board-integrated hardware with the expansion
  lifecycle. Battery ADC, PCF85063 RTC, SHTC3 sensing, FT6336 touch, ST7305,
  ILI9341, ST7796, SSD1683, CVBS PAL, VGA32, SDMMC, ES8311/ES7210,
  ES8311-duplex, and ESP32-DAC providers now use target-aware, package-gated
  expansion drivers; built-in devices register as immutable board attachments,
  while compatible hardware can use the same drivers at runtime. Added an
  MCU-first desktop board-configuration TUI and one inherited TOML manifest as
  the complete source for a custom board profile. Migrated every maintained
  built-in target to the declarative manifest system. Improved expansion
  discovery with compact aligned driver columns and separate device blocks
  with bold names. Fixed board-defined battery-monitor attachment, and corrected
  playback stream negotiation on output-only codecs, restoring ODROID-GO
  ESP32-DAC playback. `aplay` and `arecord` can now run from display, UART, USB
  CDC, Telnet, and other port shells. PlatformIO build locking now degrades to
  an explicit warning-only no-op on Windows, where POSIX `flock` is unavailable.
  Physical acceptance passed on SolarTerm (Waveshare ESP32-S3-RLCD-4.2), Freenove FNK0104S,
  Elecrow CrowPanel, ODROID-GO, and ESP32-S3 DevKitC-1; Freenove ESP32-WROVER
  v3.0 CVBS output and SDMMC storage also passed.
- **4.9.1** — 2026-08-27 — Added mirrored `solaros.input` Python and Lua
  bindings with `sources`, `read`, `clear`, and `status`. Foreground scripts can
  receive touch coordinates and actions, relative mouse deltas and buttons,
  and normalized joystick axes with source/target metadata. Each interpreter
  uses a bounded, cancellation-aware queue with overflow telemetry; keyboard
  characters remain available through `solaros.tui.getch()`.
- **4.8.16** — 2026-08-26 — Added the Freenove FNK0104S ESP32-S3 4.0-inch
  target with a landscape 480x320 ST7796 display, PWM backlight, FT6336
  capacitive touch, duplex ES8311 speaker/microphone audio, four-bit SDMMC,
  battery ADC, native USB CDC, BOOT/KEY input, and UART/I2C/GPIO expansion
  metadata. Corrected its panel color polarity. Added a typed input registry
  and `input` diagnostics for keyboards, touch, relative mice, analog
  joysticks, D-pads, and buttons. `input test` reports source capabilities,
  event counters, and the latest accepted event; `input calibrate` stores
  optional absolute-pointer coordinate mappings. Source names and calibration
  actions are available through shell completion. Added generic
  display-targeted absolute/relative pointer events and normalized axis events
  for native applications that opt in. Pointer and axis queues are allocated
  only while matching sources exist. Integrated input hardware now uses the
  expansion lifecycle: Freenove touch and the TTGO VGA32 PS/2 keyboard start as
  board attachments, while CardKB, GPIO keys, PS/2 keyboards, standard
  three-button PS/2 mice, and two-stream analog joysticks can be attached at
  runtime. Existing `ps2-keyboard` and `gpio-keys` jobs remain as compatibility
  wrappers. Cursor-based applications launched on a dumb terminal now report
  the terminal limitation directly. Standardized 16 MiB targets on two 7 MiB
  OTA slots and a nearly 2 MiB internal FAT filesystem. Existing devices retain
  their installed partition layout through normal OTA updates; adopting the
  common `/flash` layout requires a full USB/factory flash and reformats that
  volume. Completed mirrored Python/Lua continuous-control, native-parameter,
  MIDI, and OSC bindings, including runtime diagnostics, typed binding
  management, validated MIDI transmission helpers, non-consuming receive
  subscriptions, CC scalar-stream management, OSC binding telemetry, and
  bounded OSC encoding and parameter dispatch.
- **4.8.15** — 2026-08-26 — Reduced idle runtime wakeups and consolidated boot,
  bus-capability, script-lifecycle, BLE-lifecycle, JSON scanning, safe file
  replacement, and shell-completion infrastructure. Shell completion now uses
  a compact indexed rule table while preserving the existing command surface.
- **4.8.14** — 2026-08-26 — Fixed Gateway Chat reconnect replay and room join
  state. Rejoins now send the greatest retained message ID and replay only newer
  messages; conversations open after the server confirms membership. Hardened
  Messaging/Inbox clearing and transport backpressure so stale notification
  links cannot target reused entries and queued provider state is preserved.
- **4.8.13** — 2026-08-25 — Added bounded OSC 1.0 over IPv4 UDP with automatic
  incoming Synth and Funcgen parameter addresses in native units or through an
  optional final `/normalized` component, immediate bundle support, optional
  peer filtering, and named outbound scalar-stream, sampled-event, and
  normalized-control bindings on user-selected OSC addresses. Incoming MIDI CC
  values can now be registered as bounded scalar streams and mapped through
  controls to native application parameters; `midi monitor` prints incoming CC
  and key identifiers for mapping. Fixed long shell diagnostics so command
  usage is not truncated before the prompt. Added
  volatile per-display terminal profiles for secondary and virtual displays,
  including live geometry redraw without changing input focus. Expanded SDSPI
  failure diagnostics with card-command and electrical-stage details, and
  corrected the default flavor selection for affected boards.
- **4.8.10** — 2026-08-23 — Added public MeshCore hashtag channels: names
  such as `#hansemesh` derive a compatible shared key without requiring a
  Base64 pre-shared key, while explicit keys remain available for private
  channels. Fixed conventional UTC-offset handling so values such as `UTC-8`
  and `UTC+5:30` now select the expected local time. Added bounded,
  same-origin persistent HTTP/TLS sessions for Python and Lua, with exact
  origin enforcement, redirect rejection, isolated request headers, automatic
  interpreter teardown, and safe pre-response retry for GET and HEAD. HTTP
  transmit buffers now grow to fit long request headers such as bearer tokens,
  and retained connections make repeated REST calls substantially faster.
- **4.8.9** — 2026-08-22 — Python and Lua scripts can now capture bounded
  signed 16-bit PCM blocks from the default audio input, including the native
  sample rate and channel format; capture also works in generic builds with
  runtime audio expansions. Installed Playground apps become direct shell
  commands named by their app IDs, run from their installed manifests without
  loading the catalog, and stay synchronized across installs, updates, and
  removals. Added bounded asynchronous HTTP streams for Python and Lua, with
  ordered response, header, data, completion, and error events suitable for
  long-lived responses such as SSE. Script TUI input fields can mask UTF-8 text
  while editing passwords and other secrets. The MeshCore EU868 radio profile
  now uses the required 32-symbol preamble, restoring RFM95 reception with
  compatible MeshCore nodes.
- **4.8.8** — 2026-08-21 — Added selectable Espressif Long Range PHY modes to
  the ESP-NOW Link transport. Use `phy=lr500` or `phy=lr250` on participating
  devices to trade throughput for additional receive sensitivity and range;
  `phy=normal` remains the default. `espnow status` reports the active PHY, and
  stopping the job restores the previous Wi-Fi protocol selection. Added a
  runtime SSD1683 expansion for the Waveshare 4.2-inch V2 400x300 monochrome
  e-paper module, with automatic partial updates, unchanged-frame suppression,
  and periodic full refreshes to limit ghosting. Custom SSD1683 boards can now
  select `EPD_SSD1683_PANEL_WAVESHARE_V2` explicitly. Added the M5Stack Unit
  CardKB as an I2C keyboard expansion at address `0x5f`; BLE, PS/2, and I2C
  keyboards now share one input path, and the status icon reports multiple
  connected keyboards. Boards without built-in SD hardware can attach an
  SDSPI expansion and mount removable FAT storage at `/sdcard` while internal
  flash remains `/`. `cat` now adds a trailing newline when necessary so the
  shell prompt starts on a new line.
- **4.8.7** — 2026-08-21 — Added a native ESP-IDF/lwIP WireGuard IPv4 client
  with `wireguard import`, `up`, `down`, `status`, and `forget` commands. It
  supports one peer, up to eight allowed-IP prefixes, split and full-tunnel
  routing, optional tunnel DNS, Wi-Fi address-change reconnection, and
  light-sleep recovery. Full tunnels default to fail-closed leak protection;
  split tunnels default to fail-open, and either policy can be selected
  explicitly. Profiles are validated without printing key material, temporary
  secrets are wiped, handshake timestamps remain replay-safe without
  synchronized wall time, and the encrypted endpoint remains bound to Wi-Fi.
  The WireGuard worker is now created only while the tunnel is requested,
  uses the PSRAM-aware task policy where safe, and releases its task memory and
  active in-memory profile on `wireguard down`. BLE keyboard resume completes
  before WireGuard restarts after light sleep. SSH display sessions again keep
  independent local and remote text buffers; switching sessions restores the
  correct scrollback, and closing SSH returns directly to the SolarOS prompt
  without requiring an extra Enter. UART and USB CDC SSH sessions retain shared
  port scrollback.
- **4.8.6** — 2026-08-20 — Corrected italic and bold-italic font generation so
  glyph overhangs no longer lose their top-right corners at any supported text
  size. `Ctrl+V` now pastes the shared SolarOS clipboard into shell input without
  executing pasted text. Chat now uses separate Channels and Chat tabs; selecting
  a known gateway room joins it automatically before opening the conversation.
  Notes keeps the selection on the next active item after completing one, has a
  persistent help bar, can tidy all completed items, moves whole categories with
  shifted arrows, and retains its cursor while adding or editing an item. Added
  shared TUI screen layout, title, tab, help, UTF-8 input, cell, and list-navigation
  helpers across native applications and configuration TUIs, with corresponding
  high-level Python and Lua APIs. The display terminal now uses its final complete
  text row and leaves any remaining vertical space below it.
- **4.8.5** — 2026-08-20 — Added suspend mode: the primary display turns off,
  the effective power profile becomes `lowpower`, and background jobs, radios,
  Inbox handling, and audio continue. A second KEY short press resumes the
  display and restores the selected profile. Added persistent
  `setterm powerkey sleep|suspend` configuration and matching Setterm TUI,
  commands, completion, and diagnostics. New or cleared configurations now
  default to the `performance` profile, suspend as the KEY action, and Inbox
  notification sound enabled on audio-capable boards. New terminals default to
  the `compact` font and text size `16`; existing saved display preferences are
  preserved.
- **4.8.4** — 2026-08-20 — MicroPython embed generation is pinned and
  reproducible, including the SolarOS qstr and port overrides. Python now uses
  the port-compatible `EXTRA` language profile, with f-strings, sets,
  properties, descriptors, additional built-ins, and the `array`, `cmath`,
  `collections`, `errno`, `math`, and `struct` modules. Added mirrored,
  cancellation-aware `solaros.http` Python and Lua bindings for bounded HTTP
  GET, POST, PUT, PATCH, DELETE, HEAD, redirects, headers, and binary bodies.
  Added the selected `json`, `binascii`, `hashlib`, and `random` standard
  modules, including SHA-256 and hardware-seeded non-cryptographic randomness.
  Added SolarOS-backed `open()` text and binary streams plus external `.py`,
  `.mpy`, and package imports rooted at the script directory or preferred
  storage. Added mirrored, synchronous managed TCP, UDP, WebSocket, and secure
  WebSocket clients with interpreter-owned handles, bounded blocking,
  cancellation checkpoints, automatic teardown, and explicit per-runtime,
  global, timeout, and transfer limits. Python and Lua service modules now
  expand from one package-gated registration descriptor, including aliases,
  constants, and nested HID tables, so their public APIs cannot drift as typed
  bindings are extended. Native Agent scripting lookups now search bounded,
  task-specific sections of the complete firmware-matched Python and Lua
  manuals, with generated zero-copy indexes and retrieval coverage for every
  shared service. MicroPython now initializes the `EXTRA` profile's C-stack
  limit from the current FreeRTOS task, restoring REPL and Agent script
  execution while preserving recursion-depth protection. Native signed and
  unsigned 64-bit values that fit MicroPython's immediate integer range now
  convert without requiring long-integer support, restoring `solaros.http`
  response dictionaries, `solaros.uptime_ms()`, and small DSP dot products.
  Board capability output now uses one checked capacity and includes the full
  Waveshare sensor list. Enter no longer key-repeats, preventing a short Python
  app from submitting an empty command after it returns to the shell prompt.
- **4.8.3** — 2026-08-12 — Gateway Chat now uses the global SolarOS user and
  hostname identity instead of a separate saved username. Added CRC-verified
  NVS backup and restore to disk with `/.solar/nvs.bin` as the default file.
- **4.8.2** — 2026-08-12 — Synth now adapts to compact OLED and LCD targets
  with a parameter HUD, large values, level bars, live waveform and
  wavetable graphs, single-slot preset browsing, and immediate feedback from
  external control bindings. Added a runtime PCM5102A I2S expansion that
  registers a stereo audio device and exclusive playback stream. Runtime audio
  outputs now follow shared global volume, and bounded sink writes improve PCM
  playback reliability. Synth correctly highlights MIDI notes, prioritizes
  note input over compact-display refreshes, and no longer rewrites the custom
  wavetable when switching tabs. Named I2C buses can change speed at
  runtime from the `i2c` command or IO. Help and Flash trees start folded and
  retain their navigation state; Flash can delete cached artifacts. Files shows
  measured transaction progress, restores the directory just exited, and lets
  `Esc` cancel copy, move, or recursive delete without closing the app. Port
  shells coalesce line redraws so typing over Telnet keeps a stable cursor.
- **4.7.6** — 2026-08-10 — Added `funcgen`, an audio function generator with
  GUI/TUI interfaces, a live oscilloscope, selectable runtime playback streams,
  six waveform shapes, amplitude and pulse-width controls, and repeating
  frequency sweeps. Every adjustable control is available through the shared
  parameter/control-binding system. Added the focused `rover-synth` flavor for
  BLE/MIDI-controlled Synth with an attached LEDC PWM audio output. BLE keyboard
  pairing and forgetting now wait for an active HID open to finish, preventing
  connected keyboards without input and disconnect-time resets.
- **4.7.5** — 2026-08-10 — Mixed-interface applications now accept `--tui`
  to force their text interface on a graphical shell. This applies to Calc,
  Player, WebRadio, and Recorder. Added an ESP-NOW transport for SolarOS Link,
  with a managed background job, persistent and learned peer mappings, Wi-Fi
  channel coexistence, shell diagnostics, and completion. Wi-Fi driver runtime
  configuration no longer duplicates SolarOS profiles in the shared NVS
  partition, so a full NVS partition cannot block SoftAP or ESP-NOW startup.
- **4.7.4** — 2026-08-10 — Display-capable systems now allocate port-shell
  workers on demand, leaving the 16 KiB shell stack available until a UART or
  USB CDC shell is opened. Headless systems retain a reserved worker for their
  primary interactive console. Synth is now available on headless systems with
  runtime audio outputs and supports `synth --headless` for operation without
  its graphical interface.
- **4.7.3** — 2026-08-10 — Added `recorder`, an interactive GUI/TUI WAV
  recorder with capture-stream selection, mono/stereo recording, selectable
  sample rate and resolution, hardware input gain, output volume, live
  monitoring, pause/stop/playback controls, automatic filenames, an integrated
  folder browser, persistent settings, and cassette, oscilloscope, and spectrum
  views. Player, WebRadio, and Recorder now share standard graphical media
  controls. WAV playback converts channel count and sample rate to the selected
  output stream.
- **4.7.1** — 2026-08-09 — Added a persistent `setterm statusbar show|hide`
  setting. Hiding the graphical shell status bar gives its space to terminal
  rows; the default remains `show` when NVS has no saved setting.
- **4.6.28** — 2026-08-09 — Added `player`, an interactive WAV and MP3 player
  with a persistent playlist, file browser, GUI, and simple TUI. It supports
  pause, volume, automatic advance, background playback, and cassette,
  oscilloscope, and spectrum visualizers. WAV and MP3 files now open in Player.
- **4.6.27** — 2026-08-09 — `aplay` and `arecord` now work as normal
  command-line applications and return to the existing prompt when finished.
  `aplay` plays one file and can be stopped with `Esc`; `arecord` records until
  stopped when no duration is specified.
- **4.6.26** — 2026-08-09 — Added WebRadio with direct MP3 streaming, a
  persistent editable station catalog, GUI and TUI interfaces, volume and
  channel controls, background playback, and oscilloscope and spectrum
  visualizers. The initial catalog includes eight Nightride.fm stations.
- **4.6.25** — 2026-08-09 — Separated generic audio-device discovery from
  built-in board audio and reusable compressed-audio codecs. Audio apps now
  select dynamically registered input and output endpoints, allowing them to
  remain available on headless boards for hot-attached audio expansions while
  board codec and I2S dependencies remain hardware-gated. Moved minimp3 behind
  an incremental decoder API that file players and future network audio sources
  can share. Added endpoint volume operations, headless package coverage, codec
  tests, and updated audio, Synth, messaging, package, and app documentation.
- **4.6.24** — 2026-08-09 — Replaced the fixed scalar-stream catalog with the
  dynamic typed `service.streams` registry for scalar, event, byte, and PCM
  audio endpoints, including direction, sharing, provider/device metadata,
  ownership, format discovery, and transfer telemetry. GPIO, ADC, battery,
  environment sensors, runtime ports, microphone levels, and board audio now
  register their available endpoints at runtime. Added generic audio-device
  discovery with `audio0.capture` and `audio0.playback`; Synth, WAV/MP3
  playback, WAV recording, and raw DAQ audio capture now use those streams.
  The registry state lives in PSRAM while synchronization and audio/DMA state
  remain internal, recovering 10,752 bytes of internal SRAM. Updated shell
  diagnostics, completion, packages, manuals, and host coverage.
- **4.6.23** — 2026-08-09 — Synth now uses shared DSP block-level PCM analysis
  for oscilloscope scaling and publishes DSP-derived peak and RMS telemetry.
- **4.6.22** — 2026-08-09 — Added the shared fixed-point DSP service with
  native, Python, and Lua APIs.
- **4.6.21** — 2026-08-09 — Added persistent `setterm startup flash|sd`
  selection for the boot `.shell/startup` source. Internal flash is the default,
  including after `nvs clear`; SD-capable boards can explicitly select the SD
  copy without silently falling back between volumes. Added matching Setterm
  TUI controls, shell completion, resolved-path status, and documentation.
- **4.6.18** — 2026-08-08 — Added universal continuous controls for scalar
  streams, native app parameters, MIDI CC targets, and Python/Lua scripts;
  Synth now exposes its tunable sound controls through the shared contract.
- **4.6.16** — 2026-08-08 — Added bidirectional MIDI expansion buses and MIDI
  keyboard control to `synth`.
- **4.6.15** — 2026-08-08 — Added mono mode with glide to `synth` and released
  audio-driver memory when synth streams close.
- **4.6.14** — 2026-08-08 — Added factory and persistent user presets to
  `synth`.
- **4.6.13** — 2026-08-08 — Removed unintended `synth` filter saturation and
  added a tunable second oscillator.
- **4.6.12** — 2026-08-08 — Added native sine-wave playback and a Supersaw
  starting shape to the `synth` wavetable editor.
- **4.6.11** — 2026-08-08 — Added a resonant low-pass filter with a dedicated
  graphical filter-envelope tab to `synth`.
- **4.6.10** — 2026-08-08 — Added a graphical waveform-editor tab to `synth`
  with selectable resolution, baseline shapes, brush editing, smoothing,
  normalization, undo, immediate custom-wavetable playback, and smooth
  polyphonic voice transitions.
- **4.6.9** — 2026-08-07 — Added an eight-voice native synthesizer with a
  physical-key piano, waveform and ADSR controls, envelope graph, oscilloscope,
  and global volume control. Added matching Python and Lua APIs, including
  cooperative sequencer timing, and fixed oscillator clipping.
- **4.6.8** — Added transparent packed 1-bit bitmap and sprite drawing to the
  Python and Lua `solaros.gfx` APIs. Both runtimes expose `gfx.bitmap(...)` and
  its `gfx.sprite(...)` alias for reflective-display-friendly pixel art. Added
  `solaros.ssh_keys.public_key()` to both runtimes so scripts can share the
  default OpenSSH public key without access to private key material. Added
  bounded `solaros.storage.read_file(...)` access to both scripting runtimes.
  `playground run ID [ARG...]` now forwards script arguments to Python and Lua.
  Playground recognizes the canonical forwarded `--file PATH` script option
  and completes its paths. Shell completion now expands all fixed tokens in
  multi-token aliases.
- **4.6.7** — 2026-08-07 — Fixed `displayd` browser keyboard lag and event
  reordering by queueing input in the browser and sending ordered batches.
  Corrected the Elecrow rotary wheel direction mapping and made all three
  rotary controls emit one debounced key event on release, preventing slow
  e-paper refreshes from leaving Enter in a repeating state. The rotary press
  now uses the canonical Enter key mapping. Removed the top help bar from
  `files --launcher` while retaining its empty first row. Added `Alt+Right` and
  `Alt+Left` shortcuts, accepting either Alt key including AltGr, for cycling
  forward and backward through sessions on the locally focused display. Fixed
  German-layout AltGr translation so `AltGr+Tab` also cycles forward. Added
  `reader --pager`, where Up and Down use row-aware page navigation and retain
  the final visible layout row as the first row of the next page; Page Up and
  Page Down use the same precise overlap. Files launcher mode automatically
  uses pager mode for documents associated with Reader.
- **4.6.6** — 2026-08-06 — Added the `gpio-keys` job for mapping runtime-safe,
  active-low pull-up GPIO buttons to debounced keyboard presses through the
  normal SolarOS input dispatcher. Mappings can be supplied inline or loaded
  from a file, claim their pins atomically as `job:gpio-keys`, appear
  canonically in the IO application, and release cleanly when stopped. The job
  is included in the hardware-jobs group and all Rover flavors. Added a 10 px
  terminal text size, including regular,
  bold, italic, and bold-italic faces. The new size is available through
  `setterm textsize 10`, shell completion, the Setterm TUI, and the Edit and
  Hexedit text-size controls. Existing saved terminal text-size values remain
  compatible. Reworked local hardware input around a generic structured key
  service with press, release, held-state, modifiers, HID usage identity, and a
  shared repeat policy. BLE keyboards, fixed buttons, `gpio-keys`, joysticks,
  and ADC D-pads now use that service; existing text applications retain
  character input compatibility. Game Boy now consumes generic held state, so
  non-BLE controls support simultaneous buttons and release tracking too.
  Added named, exclusive PS/2 buses and the `ps2-keyboard` job. Runtime bus
  definitions claim CLOCK and DATA pins through the normal resource registry,
  appear in `io`, and can be restored from startup scripts. The receive driver
  validates start, odd-parity, and stop bits in a GPIO edge ISR; the job decodes
  scan-code set 2 and publishes canonical HID transitions through the generic
  input service. Python and Lua can create and inspect PS/2 buses too. BLE HID
  keyboard decoding now accepts both seven-byte report-protocol and eight-byte
  boot reports and keeps composite-device report streams separate before
  publishing their aggregate key state.
- **4.6.5** — 2026-08-06 — Added the focused `rover-retro` flavor for running
  the Game Boy emulator on the Freenove ESP32-WROVER v3.0 composite target. It
  uses the normal system-service baseline with BLE, SD, UART ports, hardware
  I/O, filesystem and maintenance apps, Log, Bridge, Wi-Fi, and SSH/SCP clients,
  while keeping the remaining network stack, expansion drivers, scripting, and
  unrelated application groups disabled.
  Game Boy now supports Rover's full 384x288 PAL canvas through an asynchronous
  direct XBM-to-scanout path with field-boundary swaps. It can compile without
  the synth dependency;
  Rover uses Peanut-GB's silent register path because composite scanout owns
  I2S0, while Waveshare `retro` retains the MiniGB APU and shared synth output.
  The 320x200 composite mode remains too short for the emulator's 320x288 image.
- **4.6.4** — 2026-08-06 — Replaced the Freenove `composite` flavor with the
  expansion-capable `rover`, `rover-python`, and `rover-lua` family. All three
  include the writing suite, networking, media, utilities, the log job, and the
  Bridge job while omitting OTA, battery monitoring, DAQ, SUMP, and the
  internal-memory-sensitive Logic app. The base Rover flavor includes games;
  the scripting variants instead add only their selected interpreter. Agent
  remains excluded because its runtime memory requirements are not viable on
  this configuration. Moved 11.7 KiB of
  persistent foreground-application state from internal SRAM to PSRAM, giving
  background jobs such as `telnetd` enough internal memory to start reliably.
- **4.6.3** — 2026-08-05 — Added `files --launcher`, a read-only single-pane
  startup menu that hides dot-prefixed entries and uses the full terminal
  height without the file-operation bars. File-opening associations now live
  in the installed app registry, including `.gb` ROMs for `gameboy`. Added
  `.sh` shell-script recognition in Files and direct shell execution through
  paths such as `./somescript.sh`. Added the initial Freenove ESP32-WROVER v3.0
  board target and its 4 MB, serial-installed `composite` Writerdeck flavor.
  Added its 384x288 monochrome PAL 625/50 display backend on GPIO25, using the
  ESP32 DAC, I2S0 DMA, field-boundary framebuffer swaps, and timing adapted from
  LovyanGFX. Added an optional centered 320x200 PAL safe-area build mode for
  small composite displays and a 240 MHz board CPU floor for stable scanout.
  Assigned the active-low GPIO0 BOOT buttons as SolarOS KEY inputs on the
  Freenove WROVER and ESP32-S3-DevKitC-1 targets. Released the unsupported
  Freenove camera signal pins for runtime GPIO, ADC, and PWM use while keeping
  GPIO25 reserved for PAL composite output.
  Virtual displays now inherit the board's main display dimensions; headless
  boards retain the 400x300 fallback. Fixed `df` listing internal flash twice
  on SD-capable boards.
- **4.6.2** — 2026-08-05 — Fixed foreground application path arguments so
  relative files are resolved from the current shell directory before the app
  session starts. This now covers editors, readers, scripts, audio, images,
  Game Boy ROMs, plots, downloads, and agent script files; `files [path]` also
  starts from the requested path, while manual references and remote SCP paths
  remain unchanged.
- **4.6.0** — 2026-08-04 — Added an experimental original Game Boy (DMG)
  emulator to the `retro` flavor for SolarTerm (Waveshare ESP32-S3-RLCD-4.2). It loads
  ROMs into PSRAM, persists cartridge RAM in adjacent `.sav` files, supports
  held and simultaneous controls from the BLE HID key state, and renders the
  MiniGB APU through the shared synth and audio services. Emulation, audio, and
  the RLCD presenter run independently; the presenter uses the panel's
  high-refresh mode and drops stale frames rather than blocking the core. The
  shell completes ROM paths after `gameboy`.
- **4.5.8** — 2026-08-04 — Replaced the variable-frequency power modes with
  four fixed operating profiles: 240 MHz `performance`, 160 MHz `balanced`,
  160 MHz light-sleep `battery`, and 80 MHz light-sleep `lowpower`. Added the
  reusable single-client synth service for bounded real-time stereo PCM
  rendering through the audio service, including owner and timing telemetry.
- **4.5.7** — 2026-08-04 — Made Notes insert new items at the selected position
  while keeping completed items in the Done section. Added a bounded
  asynchronous tone queue with shell, Python, and Lua controls, plus a
  persistent opt-in Inbox notification sound that coalesces bursts and remains
  disabled by default.
- **4.5.6** — 2026-08-03 — Generalized `com` from named UART buses to all
  bidirectional SolarOS byte-stream ports, including peer-bound Link virtual
  serial ports. Port completion now includes virtual ports, ownership errors
  identify the current consumer, and idle nonblocking reads no longer close the
  terminal.
- **4.5.5** — 2026-08-03 — Added peer-bound SolarOS Link streams that register
  as normal virtual serial ports. Link streams add session epochs, ordered
  segmentation, acknowledgements, retransmission, bounded buffering,
  backpressure, reconnect detection, shell commands, status, and completion.
  Stream protocol v2 uses piggybacked acknowledgements, response-before-ACK
  scheduling, short stream polling, and jittered retries so interactive shell
  echoes take a reliable two-frame fast path without half-duplex radio bursts
  colliding with the peer.
  Virtual ports can host normal port shells or connect to physical serial ports
  through the existing `bridge` job.
- **4.5.4** — 2026-08-03 — Refactored messaging boundaries: `chat` is now a
  provider-neutral view with optional gateway, MeshCore, Link, or exact
  conversation selection; gateway configuration and room controls moved to
  the `gateway` command; and the gateway-only worker is named `gateway-sync`.
  Added visible `outbox` and
  `messages outbox` controls for pending sends, clarified compact internal
  history versus Inbox notifications, and prevented Link message identities
  from colliding after a radio-link restart.
- **4.5.3** — 2026-08-03 — Renamed the persistent-storage shell command from
  `sd` to `disk`, so the same status, block listing, mount, and unmount surface
  covers internal flash and removable media. Added guarded FAT formatting with
  `disk format <flash|sd0|sd0pN> --force`; formatting requires an unmounted
  target, and non-empty internal flash is never reformatted automatically after
  a mount failure.
- **4.5.0** — 2026-08-02 — Added `writer`, a resumable graphical Markdown
  editor with hybrid source reveal: inactive blocks stay formatted while the
  active or selected blocks expose their exact Markdown. Added PSRAM-backed
  UTF-8 editing up to 256 KiB, bounded undo/redo, shared clipboard,
  find/replace, formatting controls, source-aware vertical navigation, and a
  blinking cursor. Saves use verified staged replacement with rollback, while
  recovery snapshots preserve cursor, scroll, and zoom state after interruption.
- **4.4.13** — 2026-08-01 — Added `calc`, a bounded scientific calculator with
  variables, one-argument functions, worksheet save/load, and a Desmos-style
  expression-and-plot view on graphical displays. Its expression editor uses a
  white background and a thin active-row outline to keep small text legible.
  The same app provides an interactive text REPL and `calc -e` one-shot
  evaluation on UART, USB CDC, Telnet, and other text-only shells.
- **4.4.12** — 2026-08-01 — Added a two-pane `hexedit` application with
  synchronized hexadecimal and ASCII editing. Extended `com` with optional
  hardware UART autobaud detection and an offset/hex/ASCII receive view while
  preserving its normal text-terminal mode, named-bus leasing, and display and
  port-shell support.
- **4.4.11** — 2026-08-01 — Added physical connector-layout views to the `io`
  application and shell commands, including compact board connectors and
  paged DevKit headers. Fixed SPI teardown for transient devices. Retained the
  working TinyUSB HID implementation as a dormant package excluded from
  standard builds because of its internal-SRAM cost, and restored the original
  USB Serial/JTAG FIFO sizing.
- **4.4.10** — 2026-07-31 — Fixed Wi-Fi and BLE radio recovery after light
  sleep, made long KEY presses replace the remembered BLE keyboard, added a
  NeoPixel expansion driver with Python and Lua bindings, added edge-triggered
  logic-analyzer capture, and let live plots request faster scheduler ticks.
- **4.4.9** — 2026-07-31 — Added an optional SolarOS Link provider for unified Chat. `radio-link
  ... chat=on` creates a broadcast conversation, discovers 32-bit Link source
  IDs as Contacts, projects incoming text without consuming the Link receive
  queue, sends packet-sized direct and broadcast text, and maps unicast
  acknowledgements and timeouts to delivery state.

- **4.4.8** — 2026-07-30 — The Agent app now exits with `Esc` as well as the
  app-exit key. SSH and SCP behave like normal command-line tools: they keep the
  current terminal contents, print their result inline, and return directly to
  the shell prompt. Playground now stores its catalog and applications in the
  visible `/playground` directory, and `playground delete` removes that
  directory, cleans up the legacy hidden location, and clears the loaded
  catalog while retaining source and storage preferences.
- **4.4.7** — 2026-07-30 — Applications and jobs can request scheduler ticks
  faster than the former fixed 25 ms cadence. Python and Lua scripts can use
  `solaros.tick_interval([ms])`, including `solaros.tick_interval(5)` for a
  5 ms cadence and `solaros.tick_interval(0)` to restore the default. The
  scheduler uses the fastest active request while preserving cooperative
  execution.
- **4.4.6** — 2026-07-30 — Fixed repeat Playground catalog refreshes on FAT
  filesystems, added a persistent flash/SD target shared by the catalog and
  default application installations, and made SD the default when available
  and no setting exists.
  The catalog tree and application details now use `i` to install or update and
  `u` to uninstall. Added `nvs status` for partition, entry, and namespace
  diagnostics, and `nvs clear` to erase all NVS-backed settings and reboot.
- **4.4.5** — 2026-07-30 — Added provider-neutral messaging, PSRAM-backed
  Contacts and Credentials services, bounded Conversations/Messages storage,
  offline provider outboxes, Inbox projection, live `messages` shell controls,
  safe Python/Lua APIs, and an offline-capable Chat client with live provider
  and contact identity handling. Added the pinned MeshCore companion provider
  with Ed25519/ECDH direct messaging, shared-key groups, ACK retries, discovery
  and trust enforcement, an RFM95-compatible adapter, an EU868 profile, a
  background job, and shell controls. Hardened MeshCore lifecycle transitions,
  contact-cache updates and blocking, credential persistence, memory use, and
  connected-device regression coverage.
- **4.4.4** — 2026-07-30 — Added transport-independent SolarOS Link v1 framing
  for text, binary, and acknowledgement messages with stable device IDs,
  broadcast delivery, CRC checks, bounded queues, duplicate suppression, and
  ACK tracking. The `radio-link` job and `link` shell commands provide managed
  packet-radio transport. The `bridge` job can now connect a bidirectional
  byte-stream port to an active Link for broadcast or acknowledged unicast
  traffic.
- **4.4.3** — 2026-07-30 — Added the RFM95W expansion driver with LoRa, FSK,
  GFSK, MSK, GMSK, and OOK modes, including packet and FIFO-stream operation,
  signal reporting, CRC, sync words, addressing, and Gaussian shaping. SPI
  devices now claim only their selected chip-select GPIO, leaving other
  candidates available for roles such as reset or data/command. Added atomic
  complete-radio profiles with built-in EU868 settings and eight persistent
  user profiles.
- **4.4.2** — 2026-07-29 — Reduced SD-card boot time by trusting the signed
  active manual catalog instead of reopening and hashing every previously
  verified Markdown page. Manual downloads and updates still verify the
  signature, archive, page sizes, and page hashes before activation.
- **4.4.1** — 2026-07-29 — Added resumable application sessions to UART, USB
  CDC, and network shells. `Ctrl+Z` suspends a resumable port application,
  `fg [id]` restores it on its owning terminal, and the global session list and
  close commands include port-owned applications. Child launches now suspend
  and return to Files or Playground correctly. Cross-shell creation, focus, and
  closure of display sessions remain unchanged. Port-session metadata and
  control queues prefer PSRAM and add no task stack per suspended app. SolarOS
  refuses to close the final interactive shell, and closing the current port
  shell no longer redraws a stale prompt before disconnecting. Closing a
  detached display application now restores a valid base context before its
  parent resumes, preventing a freed terminal from being drawn and rebooting
  the device. Playground now exits its top-level TUI with `Esc` and provides
  shell-usable `search`, `install`, and `run` subcommands with matching
  autocomplete and manual coverage. Install and run autocomplete streams
  installed application IDs directly from the loaded catalog without a second
  RAM cache. `playground run` now resolves the entry in the shell and launches
  Python or Lua directly instead of creating an intermediate Playground
  session. Opening the Playground TUI no longer refreshes an unavailable
  catalog automatically, so merely opening it never requires Wi-Fi. Successful
  refreshes now persist the catalog to flash; TUI startup and the new
  `playground reload` command load that local copy without network access.
- **4.4.0** — 2026-07-29 — Added Playground, a native foldable catalog browser
  for community Python and Lua applications. Users can search one configurable
  repository, inspect compatibility and installation state, verify and install
  hashed application archives to flash or SD, update or remove them, and launch
  the selected runtime. Added source selection in NVS, staged replacement,
  shell autocomplete, a package-aware manual entry, and the initial
  `solar_os_playground` repository contract with deterministic catalog/archive
  generation and Python/Lua examples. GitHub repository source URLs are
  normalized to their generated catalog, including values saved by 4.4.0.
- **4.3.20** — 2026-07-29 — Added an ASCII character-set mode for port-shell
  TUIs used through legacy serial terminals. `setterm charset ascii` and
  `session create shell ... --charset ascii` now replace Unicode box drawing,
  blocks, arrows, and punctuation with readable ASCII while leaving display
  sessions and UTF-8 terminals unchanged. Added matching autocomplete and
  Python/Lua session options.
- **4.3.19** — 2026-07-29 — Completed shell argument autocomplete across
  applications, commands, and job startup: app flags and values, filesystem
  operands, GPIO/logic inputs, manual references, and live conversation,
  inbox, expansion-device, port, display, stream, and job identifiers are now
  suggested where applicable. Added the previously omitted `gpio release`
  subcommand.
- **4.3.18** — 2026-07-29 — Extended `setterm palette normal|inverted` to the
  shared graphics palette so graphic apps reverse black, white, and dithered
  shades without framebuffer rewriting or controller inversion. Headless port
  shells can save the palette before an expansion-display session exists.
- **4.3.17** — 2026-07-29 — Added a persistent `setterm palette
  normal|inverted` setting that reverses terminal black and white independently
  of display-controller inversion modes.
- **4.3.16** — 2026-07-29 — Added `exit` to close the current UART, USB CDC, or
  telnet shell cleanly while leaving the built-in display shell active.
- **4.3.15** — 2026-07-29 — Added target-addressed foreground app creation
  with `session create <app> <display-target> [args...]`. Local BLE keyboard,
  board-button, joystick, and D-pad input now follows an explicit display focus
  instead of the globally foreground session; `session focus [display-target]`
  inspects or changes that assignment, and `Alt+Tab` cycles only sessions on
  the focused display. Port shells retain their own input while launching apps
  or display shells remotely. Suspended sessions no longer receive ticks or
  leak their terminal context into another app on the same display, preventing
  an auto-started display app and its backing shell from alternately rendering.
  Remote display-session creation and closure now run on the main scheduler
  rather than a telnet or port-shell task, preventing concurrent framebuffer
  and panel access from corrupting the built-in display during startup or
  teardown.
- **4.3.14** — 2026-07-29 — Gave the ESP32-S3-DevKitC-1-N16R8 two 6 MiB OTA
  slots and a nearly 4 MiB internal filesystem, replacing the 64 KiB shared
  layout so durable agent conversations and local files have practical space
  without an SD card. Fixed streamed agent responses in VT100 port shells by
  emitting CRLF newlines instead of staircase-producing bare line feeds. Made
  local-model tool use more reliable by exposing prompt-relevant operations,
  forbidding unverified success claims in the agent instructions, and feeding
  recoverable tool failures back to the model instead of aborting the request.
  Preserved prior tool results across stateless Chat Completions turns, stopped
  duplicate read-only tool loops, and accepted full-size streamed tool
  arguments from local OpenAI-compatible providers. Agent storage tools now
  resolve relative paths from the invoking shell directory, and tool failures
  show their concrete error name in the TUI. Clarified the exact
  `solaros_reference` argument shape for local models and stop advertising
  tools after the first repeated read-only call. Known policy-allowed tools
  requested outside the active schema set now activate for a schema-backed
  retry instead of failing the workflow. Added exact-path `storage_stat`
  inspection so file-existence questions do not misuse content search, and
  retry one empty stateless-provider turn instead of silently ending it.
  Restricted on-demand activation to declared workflow dependencies and made
  the distinction between exact-path metadata and content search explicit;
  path-like prompts now select `storage_stat` deterministically before fuzzy
  tool matching.
  (`bf4f600`, `6ed07f5`, `093ffac`)
- **4.3.13** — 2026-07-28 — Added bounded durable native-agent conversations
  in three flash or eight SD slots, with atomic checked records and explicit
  new/list/resume/delete operations,
  restored TUI transcripts, Responses continuation IDs, and bounded local
  history for Chat Completions. Added read-only native-agent workload
  inspection with centralized admission results, memory headroom, generations,
  failure reasons, and current resource claims. (`8de1c54`, `0374c24`,
  `8e3bdca`)
- **4.3.12** — 2026-07-28 — Raised the native agent's default tool budget to
  16 and configurable maximum to 32 calls, made the reserved final provider
  turn tool-free, and added per-request budget usage to `agent status`.
  (`281f45f`)
- **4.3.11** — 2026-07-28 — Made `doc/manual/` the comprehensive source shared
  by GitHub, the website, device help, and the native agent. Added the foldable
  `help` browser, `commands` and package-aware `man`, display-aware
  `reader`/`less` routing, and fail-safe exact-version updates through one
  catalog-authenticated manual archive with embedded fallback. (`10c39d2`)
- **4.3.10** — 2026-07-26 — Added the native resumable agent with OpenAI
  Responses reasoning, configurable tool policy and limits, progressive
  SolarOS-grounded tools, and safe script/storage development operations.
  Improved foreground switching and status rendering, and reduced Wi-Fi SRAM
  use. (`e7866d7`)
- **4.3.9** — 2026-07-26 — Moved device identity to NVS, added identity controls, and advertised the configured hostname over Wi-Fi. (`1acfe134`)
- **4.3.8** — 2026-07-26 — Improved job diagnostics with bold running rows, worker-stack requirements, and clearer waiting-versus-failed reporting; restored ODROID-GO IRAM headroom. (`0c73f0cd`, `f239ca6b`)
- **4.3.7** — 2026-07-25 — Added target-addressed routing to `displayd`. (`b9514e17`)
- **4.3.6** — 2026-07-24 — Bound display sessions to their parent framebuffers. (`9af40321`)
- **4.3.5** — 2026-07-24 — Added the `telnetd` background service. (`cc5f91ae`)
- **4.3.4** — 2026-07-24 — Marked the release that reserved internal executor stacks and queued background jobs under SRAM pressure. (`381277c7`)
- **4.3.3** — 2026-07-23 — Adopted a PSRAM-first allocation policy. (`ddd7865c`)
- **4.3.2** — 2026-07-22 — Added a reusable HTTP client service. (`d39e6c7b`)
- **4.3.1** — 2026-07-22 — Added the background chat client synchronizer. (`fb03fb7b`)
- **4.2.21** — 2026-07-21 — Cleaned up board metadata. (`ec4e9ee3`)
- **4.2.20** — 2026-07-20 — Completed adoption of the memory policy. (`cf83b313`)
- **4.2.14** — 2026-07-20 — Added cooperative-scheduling timing controls. (`17a6091d`)
- **4.2.13** — 2026-07-20 — Added paired creation and deletion helpers for external stacks. (`69bdb5bc`)
- **4.2.11** — 2026-07-20 — Added I/O autostart support that appends commands to the startup script. (`2bb7c5ca`)
- **4.2.10** — 2026-07-20 — Expanded expansion-command help and autocomplete. (`3f2afd3b`)
- **4.2.9** — 2026-07-20 — Added concurrency protection for buses. (`59de5c70`)
- **4.2.8** — 2026-07-20 — Added concurrency protection for services. (`3e6a860f`)
- **4.2.7** — 2026-07-19 — Added the I/O TUI. (`7e91364e`)
- **4.2.6** — 2026-07-19 — Kept UART descriptors registered and made them non-removable. (`f4970d73`)
- **4.2.5** — 2026-07-19 — Added named UART support. (`7d5dc797`)
- **4.2.4** — 2026-07-19 — Added a named OneWire backend and discovery. (`eab3a212`)
- **4.2.3** — 2026-07-19 — Consolidated board buses into one canonical `SOLAR_OS_BOARD_BUSES` table. (`9050aa84`)
- **4.2.2** — 2026-07-18 — Added board pin policies and atomic claim bundles. (`8c897a0c`)
- **4.2.1** — 2026-07-18 — Allowed the editor to run without PSRAM. (`be956343`)
- **4.2.0** — 2026-07-18 — Added explicit memory-allocation classes and guarded PSRAM fallback. (`398c8e36`)
- **4.1.3** — 2026-07-17 — Added the Inbox app. (`d0a1580a`)
- **4.1.2** — 2026-07-17 — Fixed startup handling and omitted icons on compact displays. (`4937eb17`)
- **4.1.1** — 2026-07-17 — Added SSD1306 and SH1106 OLED expansion-display support. (`2b8eaa28`)
- **4.1.0** — 2026-07-17 — Made internal flash the default storage when no SD card is present. (`7bc00877`)
- **4.0.0** — 2026-07-17 — Added CrowPanel 4.2-inch e-paper board support. (`f067859a`)

## 3.x

- **3.9.4** — 2026-07-16 — Changed the default OTA endpoint. (`73aed466`)
- **3.9.3** — 2026-07-14 — Applied capability gates to the Python and Lua APIs. (`46f0a8c2`)
- **3.9.2** — 2026-07-14 — Added OneWire bindings for Python and Lua. (`1697688b`)
- **3.9.1** — 2026-07-14 — Added OneWire support. (`821ab479`)
- **3.9.0** — 2026-07-14 — Added the SUMP job and Logic app. (`4a5ea682`)
- **3.8.4** — 2026-07-07 — Increased the maximum accepted OTA index size. (`921ecc54`)
- **3.8.3** — 2026-07-07 — Added wildcard support to SCP. (`02cc9c94`)
- **3.8.2** — 2026-07-07 — Added SD-card unmount support. (`859527d4`)
- **3.8.1** — 2026-07-07 — Published a documentation-only maintenance release. (`80aba5b2`)
- **3.8.0** — 2026-07-04 — Added SIMD engine API plumbing. (`5e4ac543`)
- **3.7.0** — 2026-07-03 — Split display targets. (`0b0f631a`)
- **3.6.1** — 2026-07-03 — Added terminal-capability handling to port shells. (`ca9d0dba`)
- **3.6.0** — 2026-07-02 — Cleaned up root-path and default-storage handling. (`4331013c`)
- **3.5.0** — 2026-07-02 — Added expansion capabilities and the expansion service. (`36e3356c`)
- **3.4.0** — 2026-06-30 — Added D-pad support. (`f79f8d30`)
- **3.3.1** — 2026-07-01 — Fixed I2S DMA allocation. (`f806023e`)
- **3.3.0** — 2026-06-29 — Added RAMFS. (`51875f03`)
- **3.2.0** — 2026-06-29 — Restored child-session support. (`78215a4b`)
- **3.1.0** — 2026-06-29 — Added the Telnet foreground app. (`b21ad0c0`)
- **3.0.0** — 2026-06-29 — Added TUI caching. (`9a67767a`)

## 2.x

- **2.9.8** — 2026-06-29 — Added package-tree output. (`8641b601`)
- **2.9.7** — 2026-06-28 — Added note categories. (`4eae673e`)
- **2.9.6** — 2026-06-28 — Changed SCP's default-target behavior. (`097579e2`)
- **2.9.5** — 2026-06-28 — Added filename tab completion. (`b459c7ff`)
- **2.9.4** — 2026-06-28 — Closed SSH session requests when setup fails. (`93989048`)
- **2.9.3** — 2026-06-28 — Exposed the graphics capability to Lua and Python. (`231382b1`)
- **2.9.2** — 2026-06-27 — Adjusted OTA task stack sizing. (`bd616761`)
- **2.9.1** — 2026-06-27 — Reduced OTA heap pressure and adjusted the shell stack. (`288aed73`)
- **2.9.0** — 2026-06-27 — Added cooperative foreground sessions. (`f5d7cac4`)
- **2.8.0** — 2026-06-27 — Added power management. (`db30d271`)
- **2.7.3** — 2026-06-27 — Shut down the radio before sleep. (`2b822b57`)
- **2.7.2** — 2026-06-27 — Added shell terminal probes. (`e1cd39e5`)
- **2.7.1** — 2026-06-27 — Allowed SSH to run without storage. (`9cd7509a`)
- **2.7.0** — 2026-06-27 — Added the JSON service. (`633ee69e`)
- **2.6.1** — 2026-06-27 — Added OTA schemas. (`71f3b6d1`)
- **2.6.0** — 2026-06-27 — Added the board-capability layer. (`0c04bf46`)
- **2.5.1** — 2026-06-27 — Added the item editor to Notes. (`8646a2a3`)
- **2.5.0** — 2026-06-26 — Added the Files commander app. (`a477d815`)
- **2.4.2** — 2026-06-26 — Added the splash screen. (`d90f79cb`)
- **2.4.1** — 2026-06-26 — Added the document asset provider. (`2fa610ec`)
- **2.4.0** — 2026-06-26 — Added ZIP support. (`1460d9ac`)
- **2.3.0** — 2026-06-25 — Added the Xfer app. (`46aea50f`)
- **2.2.1** — 2026-06-25 — Added the Notes app. (`47ef9b7d`)
- **2.2.0** — 2026-06-24 — Added the Plot app. (`8fcf2c1e`)
- **2.1.1** — 2026-06-24 — Returned to single BLE-keyboard support. (`5bea632a`)
- **2.1.0** — 2026-06-24 — Added the Sheet app. (`986e9ab2`)
- **2.0.1** — 2026-06-24 — Added multistream data acquisition. (`5095c8e2`)
- **2.0.0** — 2026-06-24 — Added the stream service and data-acquisition job. (`218fd988`)

## 1.x

- **1.9.8** — 2026-06-24 — Added deep-shell support. (`fa18b979`)
- **1.9.7** — 2026-06-24 — Added remembered Wi-Fi networks. (`685456cf`)
- **1.9.6** — 2026-06-24 — Added monochrome xterm handling for SSH. (`87b88bf2`)
- **1.9.5** — 2026-06-24 — Added the battery monitor. (`2e818541`)
- **1.9.4** — 2026-06-24 — Fixed UART status reporting. (`37385638`)
- **1.9.3** — 2026-06-24 — Integrated Lua and BLE support. (`840d2916`)
- **1.9.0** — 2026-06-23 — Added the Lua REPL. (`1980162f`)
- **1.8.0** — 2026-06-23 — Added the power service and shell controls. (`a35fd558`)
- **1.7.5** — 2026-06-23 — Made OTA updates flavor-aware. (`a817bc5d`)
- **1.7.0** — 2026-06-23 — Added build packaging and firmware flavors. (`b068e725`)
- **1.6.0** — 2026-06-23 — Added MP3 playback. (`4c933216`)
- **1.5.0** — 2026-06-23 — Added the web browser. (`a2878970`)
- **1.4.1** — 2026-06-22 — Optimized the chat app and service. (`dc0d4c7f`)
- **1.4.0** — 2026-06-22 — Added the initial work-in-progress chat gateway. (`795c39cb`)
- **1.3.1** — 2026-06-22 — Fixed battery charge-state reporting. (`72582901`)
- **1.3.0** — 2026-06-22 — Added the MQTT service and shell/Python integration. (`52076d78`)
- **1.2.1** — 2026-06-22 — Added global audio-level control. (`e87a53b8`)
- **1.2.0** — 2026-06-21 — Added the memory service and integrated it with the shell, Python runtime, and SLIP job. (`d779a790`)
- **1.1.0** — 2026-06-21 — Added the SLIP background job. (`cdb85cb6`)
- **1.0.0** — 2026-06-21 — Established the initial SolarOS firmware, services, shell, apps, jobs, MicroPython runtime, and Waveshare ESP32-S3 RLCD board support. (`7ab94ab9`)
