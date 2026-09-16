# LilyGO T-Deck Plus port

This is the port-maintainer reference for the `t_deck_plus` board. The board
manifest is the source of truth for its wiring and built-in attachments.

## Board model and integration rules

The LilyGO T-Deck Plus uses an ESP32-S3FN16R8 (16 MiB flash and 8 MiB octal
PSRAM), a landscape 320x240 ST7789 display, shared SPI/I2C buses, raw-matrix
keyboard, GT911 touch, GPIO trackball, SPI microSD, I2S playback/capture,
battery ADC, and SX1262 radio.

The port declares built-in peripherals through the expansion subsystem where
a reusable driver exists. That is deliberate: manifests provide concrete
wiring; driver descriptors provide capability, binding, probe, attach/detach,
and resource-ownership validation. A driver must not hard-code T-Deck pins.

## Platform and transport drivers

| Driver | Configuration | Contract |
| --- | --- | --- |
| `cdc_usb_serial_jtag` | Native ESP32-S3 USB | USB Serial/JTAG console; separate from UART and input peripherals. |
| `uart_esp_idf` | UART1, TX GPIO43, RX GPIO44, 9600 baud | External serial port via exclusive `uart1`; runtime retains UART0 and UART2. |
| `i2c_esp_idf` | I2C0, SDA GPIO18, SCL GPIO8 | Owns shared `i2c0`; keyboard, touch, and microphone bind by name rather than initializing I2C themselves. |
| `spi_esp_idf` | SPI2/FSPI: SCLK 40, MISO 38, MOSI 41, 4 KiB transfers | Owns shared `spi0`; LCD, SD, and radio declare independent CS resources. |
| `gpio_esp_idf` | Board GPIO | Common GPIO implementation for fixed board pins and expansion bindings. |
| `adc_esp_idf` | GPIO4 | ADC implementation for battery and future ADC expansion. |
| `pwm_esp_idf` | Board PWM capability | Optional PWM service. LCD backlight is currently fixed active-high GPIO, not PWM. |

## Display, storage, power, and radio

### `display_st7789` / `st7789`

The board display backend is `display_st7789`; built-in `display0` uses the
reusable `st7789` attachment. It binds `spi0`, CS GPIO12, DC GPIO11,
backlight GPIO42, and peripheral power GPIO10. MADCTL `0x60`, the 320x240
post-rotation window, and `U8G2_R0` align SolarOS landscape coordinates with
the physical panel. The driver owns display transactions and any DMA staging;
the bus layer owns the SPI host. Hardware buffers follow the memory policy.

### `storage_expansion` / `sdspi`

`storage0` attaches reusable SPI microSD to `spi0` with CS GPIO39.
LCD and SD share peripheral power GPIO10. Storage claims only its declared SPI
device and uses DMA-safe transfer storage where required; it does not own LCD
or radio resources.

### `battery_adc` / `battery-adc`

`battery0` measures GPIO4 through the declared `2000` divider (2.0x). The
board battery service supplies battery state; the attachment supplies its
physical binding and diagnostics.

### `sx1262`

`radio0` is a reusable SX1262 attachment on `spi0`: CS GPIO9, BUSY GPIO13,
reset GPIO17, IRQ GPIO45. Separate bindings let expansion validate and claim
each resource. The optional radio cannot disturb display, SD, or keyboard use.

## Input drivers

### `tdeck-keyboard`

This is a reusable expansion driver, not a CardKB fork. It requires a named
I2C bus and address `0x55`, probes the controller, selects raw-matrix mode,
polls five columns every 10 ms, and emits newly pressed keys through a keyboard
input source. Detach stops and joins its worker, restores controller key mode,
closes the input source, and releases the attachment.

The generic driver supplies a 5x7 normal/symbol matrix map;
`SOLAR_OS_BOARD_TDECK_KEYMAP` permits a board-specific keycap/wiring override.
It reports Alt, Symbol, and Shift modifier state, capitalizes ASCII letters
with Shift, and has no opinion about application commands such as Back or Quit.

### Input composition and board chords

`solar_os_input_composition` is the policy layer between sources and the
shared input service. It records modifiers per source and applies the board's
`SOLAR_OS_BOARD_INPUT_CHORDS` before publishing the key.

| Physical chord | Canonical result | Intent |
| --- | --- | --- |
| Alt + trackball up | Page Up | Scrollback/page up |
| Alt + trackball down | Page Down | Scrollback/page down |
| Alt + Q | app-exit | Substitute for `Ctrl+Alt+Del` |
| Alt + C | Escape | Back/cancel |
| Alt + S | `Ctrl+S` (`0x13`) | Save in terminal-style apps |

Outputs are canonical SolarOS keys, so normal foreground applications receive
consistent semantics from local, USB/BLE, and future input sources.

### `solar_os_buttons` (trackball)

The trackball uses fixed, active-low GPIO buttons: up GPIO3, down GPIO15,
left GPIO1, right GPIO2, center GPIO0. Events emit on stable release. The
common service adds 25 ms debounce and the manifest adds a 90 ms horizontal
guard after vertical release, suppressing accidental left/right impulses while
scrolling. Center remains Enter; GPIO0 is also a boot-strapping pin and is not
repurposed as reset.

### `pointer_gt911` / `gt911`

`touch0` binds GT911 to `i2c0`, address `0x5d`, IRQ GPIO16, rotation `1`.
The reusable driver accepts both common strap addresses (`0x5d` and `0x14`).
It publishes absolute pointer events to shared input, and rotation matches the
landscape display.

## Audio drivers

### `audio_expansion` / `i2s-output`

`audio0` is generic Philips-I2S playback: I2S0, BCK GPIO7, data GPIO6,
word-select/RCK GPIO5. `i2s-output` is the neutral alias for the reusable
PCM5102-style driver. Explicit `i2s=0` is essential: expansion owns the I2S
claim; the driver opens the port passed by the binding. This keeps it reusable
and prevents undeclared I2S resource claims.

### `es7210`

`mic0` uses ES7210 control on `i2c0` and capture on I2S1: MCLK GPIO48,
BCK GPIO47, WS GPIO21, data-in GPIO14. I2S1 isolates capture from speaker
playback. The driver requires I2C, I2S, and every signal binding before attach.

## Lifecycle, memory, and expansion compliance

Each attachment has matching native binding specs and catalog metadata:
capabilities, binding kinds, address allowlists, GPIO roles, and parameter
ranges are validated before attach. The keyboard worker uses SolarOS's
background task API with an internal stack and is stopped before its input
source closes. The port contains no foreground app state or preallocated app
memory; display and other hardware buffers use their documented memory-policy
exceptions.

## Known upstream issue: Ping and local app-exit

Alt+Q maps correctly to `SOLAR_OS_KEY_APP_EXIT` (`0x92`). Normal foreground
apps receive it through the shared input service and session dispatcher.

Ping runs synchronously and its `shell_read_app_exit_key()` cancellation
callback polls only the BLE keyboard byte source and, for a port shell, that
port's input handle. It never reads the shared local input source where the
T-Deck driver publishes Alt+Q. Thus Alt+Q cannot stop a local foreground Ping.
This is a generic Ping input-architecture gap exposed by the T-Deck, not a
keyboard, keymap, or chord-mapping defect. Until fixed, use a finite command,
for example `ping example.com 4`.

The appropriate upstream repair is a session-safe cancellation path for
synchronous commands. Ping must not consume the global input queue directly,
because that would race the normal session dispatcher.

## Configurable shortcuts without weakening driver design

The current chord list is compile-time board policy: reliable essential access
on a small keyboard and a safe fallback if settings storage is unavailable.
It is not yet a user-facing runtime remapping API.

The compatible design is:

1. Keep `tdeck-keyboard` hardware-only: matrix scan, key normalization, and
   modifier reporting. It must not read NVS, parse preferences, or know apps.
2. Extend `solar_os_input_composition` into a bounded chord-map service. Start
   with board defaults, overlay validated persistent user rules, and expose
   list/add/remove/reset/persist commands (and later a TUI).
3. Accept only canonical input keys, modifier masks, and canonical output keys;
   reject duplicates/conflicts and bound storage. Keep a non-removable recovery
   path or ensure USB/UART can always issue `reset`.
4. Have every keyboard-like driver use this same layer, as the T-Deck keyboard
   and board buttons already do.

That makes remapping an OS policy feature. Expansion drivers stay reusable,
board defaults remain declarative, and user preferences no longer require a
firmware fork.
