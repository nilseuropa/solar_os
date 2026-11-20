+++
id = "watcher"
title = "SenseCAP Watcher"
section = "hardware"
summary = "Board devices, expansion connectors, and display console for Seeed Studio SenseCAP Watcher"
aliases = ["sensecap-watcher", "seeed_sensecap_watcher"]
keywords = "seeed sensecap watcher spd2010 pca9535 es7243 es8311 grove connectors"
packages_any = []
+++
# SenseCAP Watcher

Build the ESP32-S3 firmware with `pio run -e seeed_sensecap_watcher`. The target
uses 32 MB QSPI flash, 8 MB octal PSRAM, and the `full` flavor. Its partition
table provides two 7 MiB OTA slots and almost 18 MiB of internal FAT storage.

SolarOS starts its shell on the display. Use a paired BLE keyboard for typing;
hold the wheel push button to start pairing.

Use the **bottom USB-C** programming port. The rear USB-C port supplies power
only. The dual USB-to-UART bridge presents an ESP32 port and a separate Himax
port; use the ESP32 port at 115200 baud. For an optional UART shell, run
`session create shell uart0` from the display shell.

Native USB starts **off**, leaving expansion GPIO19/20 free for
GPIO, ADC, PWM, and runtime buses. With the USB package included, use
`usb mode device` for native CDC on J2, `usb mode host` for USB peripherals,
and `usb mode off` to release the pins again. Release any pin or `cdc0` owner
before switching. These native USB signals are separate from the USB-to-UART
programming connector. See [USB modes and wiring](usb.host.md).

The panel is circular, with 412x412 addressable pixels. The default layout
canvas is a centered 291x291 rectangle. Terminal and TUI content fits this
rectangle; graphics may draw into the surrounding panel pixels using canvas
coordinates. `display canvas display0` reports both bounds, and
`display canvas display0 full` temporarily uses a 412x412 layout canvas.

Identify the ESP32 port by its `ESP-ROM:esp32s3` startup message. Serial-port
numbering varies; the other interface belongs to Himax and runs at 921600 baud.

On Linux, disable hang-up-on-close on the identified ESP32 port before opening
the terminal. For example, if that port is `/dev/ttyACM1`:

```sh
stty -F /dev/ttyACM1 -hupcl
pio device monitor -e seeed_sensecap_watcher -p /dev/ttyACM1
```

The Watcher environment keeps DTR and RTS asserted together. Both lines feed
the hardware reset/boot circuit; changing them separately can reboot the ESP32.
Keep hardware flow control disabled and close other monitors before connecting.
The Linux hang-up setting must be applied again after reconnecting USB.

## Board devices

These devices attach automatically with `origin=board` and reserve their
wiring. Inspect them with `expansion devices`, `board`, `io`, and `input status`.

| Device | Driver | Wiring |
| --- | --- | --- |
| `gpiox0` | PCA9535 | `i2c0`, address `0x21`, SDA47/SCL48 |
| `camera0` | Himax HX6538 | SPI2 shared with SD, CS21, ready `gpiox0:6`, reset `gpiox0:7`, power `gpiox0:11` |
| `display0` | SPD2010 | SPI3 QSPI, CLK7, D0=9, D1=1, D2=14, D3=13, CS45, backlight8 |
| `touch0` | SPD2010 touch | `touch-i2c`, address `0x53`, SDA39/SCL38, IRQ `gpiox0:5` |
| `storage0` | SDSPI | SPI2, CLK4, MOSI5, MISO6, CS46, power `gpiox0:8` |
| `audio0` | ES8311 + ES7243E | `i2c0`, DAC `0x18`, ADC `0x14`; I2S0 MCLK10/BCLK11/WS12/DIN15/DOUT16; amplifier `gpiox0:12` |
| `battery0` | Battery ADC | GPIO3, divider ratio 4.1; 400 mAh capacity |
| `rtc0` | PCF8563 | `i2c0`, address `0x51` |
| `wheel0` | Rotary encoder | Logical A42/B41; Up/Down detents |
| `system-key` | System KEY | Active-low wheel push button `gpiox0:3`; short press Enter, long press BLE pairing |
| `pixels0` | One WS2812 RGB LED | GPIO40 |

The expander attaches first. It enables the system latch, SD, LCD, Grove,
battery divider, and the factory AI power rail, while leaving the speaker
amplifier disabled until audio output starts. Touch uses its own I2C bus.

The wheel push button uses the system KEY policy. Its default long-press
threshold is 1.2 seconds; releasing a long press does not also send Enter.
Change either action with `power key short <action>` or `power key long <action>`.
The button also wakes software suspension, light sleep, deep sleep, and
shutdown. The PCA9535's shared active-low interrupt output connects to ESP32
GPIO2, an RTC-capable wake input. Light sleep resumes the running session;
deep sleep and shutdown wake into a fresh boot. Holding the wheel during wake
does not also send Enter or start pairing. Other input changes on the same
expander can also assert this shared wake line.
The panel output and backlight turn off before sleep. Its shared touch supply
stays enabled at shutdown to preserve a stable wake interrupt.

Some Watcher revisions fit **ES7243 at `0x13`** rather than ES7243E at `0x14`.
For those boards, change `audio0` to `driver = "es8311-es7243"`, `addr = 0x13`,
and set `SOLAR_OS_BOARD_AUDIO_CODEC_IN` to `"ES7243"` in the board manifest.
The ES8311 DAC remains at the 7-bit address `0x18` in either revision.

The [Himax camera driver](himax-hx6538.md) samples the OV5647 through the factory
SSCMA firmware. Use `camera resolutions` to list its modes and
`camera capture /sample.jpg 416x416` to save a JPEG. The default is 240x240;
416x416, 480x480, and 640x480 are also available. Camera sampling preserves the
Himax firmware and installed models. The camera driver supplies sampling;
the included `processor-himax` provider supplies
[model management](himax-models.md) and [inference pipelines](pipelines.md).

Battery percentage is estimated from voltage. The standalone charger operates
in hardware; this profile does not expose charging-current control.

## Expansion connectors

Positions below are **schematic pin numbers**, not a guessed front/back view.
J2 has pins 1..4 opposite pins 8..5. `io` displays the same connector map.

| Connector | Pin | Signal |
| --- | --- | --- |
| J2 | 1 | GND |
| J2 | 2 | 3.3 V output |
| J2 | 3 | I2C SDA, GPIO47 |
| J2 | 4 | GPIO19 / native USB D- |
| J2 | 5 | GPIO20 / native USB D+ |
| J2 | 6 | I2C SCL, GPIO48 |
| J2 | 7 | 5 V input |
| J2 | 8 | GND |
| J5 Grove | 1 | I2C SCL, GPIO48 |
| J5 Grove | 2 | I2C SDA, GPIO47 |
| J5 Grove | 3 | Switched 3.3 V output (`gpiox0:14`) |
| J5 Grove | 4 | GND |

J2 and J5 share `i2c0` with onboard devices. GPIO47/48 stay owned by that bus;
attach I2C expansions through `i2c=i2c0`. GPIO19/20 are free 3.3 V GPIO, ADC,
and PWM signals while native USB is off. Host/device mode claims both pins.
USB host use requires suitable external peripheral power; J2's 5 V contact is
a supply input, not a software-controlled VBUS output or GPIO.

Hardware references: [Seeed's hardware overview](https://wiki.seeedstudio.com/watcher_hardware_overview/)
and [SenseCAP Watcher BSP](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/tree/main/components/sensecap-watcher).

## Quick reference

Target: `seeed_sensecap_watcher`. Console: display with BLE keyboard input.
Optional UART shell: `session create shell uart0`, bottom USB-C ESP32 port,
115200 baud. Display/touch: SPD2010, 412x412. I2C expansions: `i2c0`, SDA47,
SCL48. Free header GPIOs at boot: 19 and 20. Native USB: `usb mode off|device|host`,
default off. Camera: `camera0`, factory Himax JPEG sampling.
