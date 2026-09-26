+++
id = "expansion"
title = "Expansion drivers and attached devices"
section = "hardware"
summary = "Discover, attach, and detach package-gated expansion devices"
aliases = ["devices", "drivers", "expansion-export", "ssd1683", "epaper", "e-paper", "st7305", "ili9341", "st7796", "st7789", "cvbs", "pal", "vga32", "cardkb", "tdeck-keyboard", "keyboard", "tca8418", "rotary-encoder", "ft6336", "gt911", "mgc3130", "skywriter", "sdmmc", "sdspi", "micro-sd", "pcf85063", "shtc3", "battery-adc", "max17048", "audio-pwm", "ledc-audio", "pcm1808", "i2s-adc", "pcm5102", "pcm5102a", "i2s-output", "i2s-dac", "es8311", "es7210", "esp32-dac", "rfm69", "rfm69h", "rfm95", "sx1262", "ublox-mia-m10q", "st25r3916", "bhi260ap", "drv2605", "bq25896", "bq27220", "xl9555", "neopixel", "ws2812", "lora", "fsk", "gfsk", "msk", "gmsk", "ook"]
keywords = "python lua expansion device driver category attach detach save startup export manifest snapshot custom board bindings display epaper e-paper ssd1683 st7305 ili9341 st7796 st7789 cvbs pal composite vga vga32 waveshare cardkb m5stack tdeck keyboard tca8418 rotary encoder quadrature mouse joystick pointer ft6336 gt911 mgc3130 skywriter 3d gesture airwheel input i2c sd sdmmc sdspi microsd storage oled lcd rtc pcf85063 sensor shtc3 peripheral battery battery-adc fuel gauge max17048 bq27220 charger bq25896 audio pwm ledc pcm1808 adc pcm5102 i2s-output es8311 es7210 esp32 dac i2s radio rfm69 rfm69h rfm95 sx1262 modem sim7670 gnss ublox mia-m10q nfc st25r3916 imu bhi260ap haptic drv2605 gpio expander xl9555 neopixel ws2812 rgb led strip fsk gfsk msk gmsk ook lora"
packages_any = ["service_expansion"]
+++
# Expansion drivers and attached devices

Expansion drivers turn named buses and safe GPIO slots into active displays,
radios, sensors, or manual resource profiles. Drivers are package-gated, so the
available list depends on the firmware and board.

Control-line bindings such as device power may use either a native ESP GPIO
number or a named GPIO-controller line such as `gpiox0:12`. Expanded GPIO lines
are digital control resources only. They cannot replace native ESP pins in SPI,
I2C, UART, I2S, PWM, interrupt, or other bus/peripheral-signal bindings.

Integrated hardware uses the same composition model. A board profile declares
its fixed buses and default attachments, which are created at boot, shown by
`expansion devices`, and cannot be detached. For example, Freenove `touch0` is
an `ft6336` attachment; Waveshare `rtc0` and `environment0` use `pcf85063` and
`shtc3`. Battery boards expose `battery0` through `battery-adc`, `max17048`,
`bq27220`, or an integrated controller adapter. TTGO VGA32 `keyboard0` is a
`ps2-keyboard` attachment. Built-in audio also appears as `audio0`: SolarTerm
uses `es8311-es7210`, Freenove uses
`es8311-duplex`, T-Deck Plus uses `i2s-output` with a separate `es7210` capture
device, and classic ESP32 audio boards use `esp32-dac`. CL-32 declares its
integrated AVR as fixed `core0`; its polled event FIFO supplies the `keyboard0`
input source and its voltage and power-status registers supply `battery0`.
Generic input, time, sensor, battery, and audio services consume the same
runtime providers whether the attachment came from the board profile or the
shell.

Built-in displays follow the same rule and appear as fixed `display0`
attachments: Waveshare uses `st7305`, Freenove uses `st7796`, ODROID-GO uses
`ili9341`, Elecrow CrowPanel boards use `ssd1683`, ESP32-WROVER v3.0 uses `cvbs-pal`,
T-LoRa-Pager uses `st7796`, T-Deck Plus uses `st7789`, and TTGO VGA32 uses
`vga32`. They attach before the splash and primary display service start.

Built-in SDMMC slots also appear as fixed `storage0` attachments. SolarTerm,
Waveshare ESP32-S3-SIM7670G-4G, and ESP32-WROVER v3.0 use one-bit bindings;
Freenove uses four-bit bindings. The attachment claims and configures the pins
early, while the normal storage phase still probes and mounts the card. The
Waveshare 4G target also registers fixed `battery0`, `pixels0`, and `modem0`
attachments through the MAX17048, NeoPixel, and SIM7670 drivers.

Elecrow CrowPanel, CL-32, T-LoRa-Pager, and T-Deck Plus use fixed `sdspi`
attachments instead. Their built-in SPI devices share named buses while
claiming separate chip-select resources.

Named MIDI connections are created as buses rather than attached drivers. Use
`expansion bus create midi <name> tx=<gpio> rx=<gpio>`; SolarOS chooses the UART
backend and the `midi` background job owns the connection while it runs.

## Discover what is present

```text
expansion
expansion status
expansion drivers
expansion devices
display list
```

`expansion` opens the device manager. Its Devices view lists current
attachments and opens their details; detachable runtime devices can be removed
after confirmation. Its Drivers view groups drivers by category and opens an
attachment form for drivers supported by the running board. Binding forms use
existing named buses; create, attach, detach, or remove buses in the `io` app.
`expansion status` retains the textual capabilities, buses, devices, and claims
report for scripts and terminal inspection.

`expansion drivers` groups compiled drivers under bold Audio, Display, Input,
Power, Radio, Sensor, Storage, and Utility headings, with driver names sorted
inside each category. Its aligned rows also show probe support, bus type, and
the driver summary. `expansion devices` prints each attached device in a
separate block, with its name in bold followed by origin, readiness, startup
mode, attachment policy, and bindings.

From a script, inspect `solaros.expansion.drivers()` and
`solaros.expansion.devices()`. A driver existing in firmware does not mean a
physical device is attached.

## Attach deliberately

Use real bus and pin names returned by discovery:

```text
expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5
display test lcd0
expansion detach lcd0
```

Detaching releases the resources. Do not invent a target name or copy bindings
from a different board.

To restore a runtime attachment after reboot, open its device detail in
`expansion` and press `S`. SolarOS reconstructs the normalized `expansion
attach` command and adds it once to the startup script selected by `setterm
startup`. If the device uses a runtime bus, save that bus for startup from
`io` first so its `expansion bus create` command appears earlier in the script.
Compiled board devices already start automatically and do not need a saved
command.

When an experimental wiring arrangement becomes permanent, export its hardware
description instead of copying startup commands:

```text
expansion export /sdcard/my-hardware.toml
```

The versioned `solaros-expansion` manifest contains the current board ID plus
runtime-created buses and runtime-attached catalog devices. References to
board-owned buses remain references, so the base board hardware is not
duplicated. Readiness, leases, service state, secrets, and shell startup
commands are not part of the file. A `manual` attachment cannot be exported
because it has no catalog driver contract that the board compiler can validate.
The destination is written through a temporary file and atomically replaced.

The export format is a strict TOML contract. Schema 1 has this shape:

```toml
schema = 1
kind = "solaros-expansion"

[base]
board = "waveshare_esp32_s3_sim7670g_4g"
firmware = "4.13.1"

[[buses]]
name = "spi0"
protocol = "spi"
sharing = "shared"
host = "SPI2_HOST"
sclk = 7
mosi = 8
miso = 3
cs = [9]
max_transfer_size = 4096

[[devices]]
driver = "ssd1683"
name = "display0"
bindings = { spi = "spi0", cs = 9, dc = 10, reset = 11, busy = 12 }
```

`base.board` selects the manifest that the generated target inherits;
`base.firmware` records export provenance and is not a compatibility gate.
Only runtime-created I2C, SPI, UART, MIDI, 1-Wire, and PS/2 buses appear in
`buses`. Each bus carries its concrete controller and protocol-specific wiring.
Only runtime catalog devices appear in `devices`; each device carries the
normalized string or integer bindings accepted by its driver. Board-owned
buses may still be referenced by an exported device without being duplicated.
The importer rejects unsupported schema versions, unknown fields, duplicate
names, missing base boards, unavailable controllers or pins, unknown drivers,
invalid bindings, and conflicts with existing board resources. Treat the file
as generated interchange data; make lasting hardware changes in the resulting
board manifest.

Move the file to a SolarOS source tree and run:

```sh
python3 scripts/board_config.py --expansion-manifest my-hardware.toml
```

The configurator loads the recorded base manifest, asks for the new board
identity, validates the combined pin and bus ownership, and writes a small
inherited board profile. Imported hardware then starts as board-owned after the
custom target is built and flashed. Remove any matching temporary startup
commands yourself; they are intentionally outside the export contract.

A Waveshare 4.2-inch V2 monochrome e-paper module uses the SSD1683 expansion
driver and registers a 400x300 display target:

```text
expansion attach ssd1683 epd0 spi=spi0 cs=gpio10 dc=gpio17 reset=gpio16 busy=gpio15
display test epd0
display mode epd0 refresh=fast
expansion detach epd0
```

The runtime defaults are the Waveshare V2 panel profile, 2 MHz SPI, and
rotation 0. Optional `power=<gpio>`, `clock=<khz>`, `rotation=<0..3>`, and
`panel=<0..4>` bindings adapt the same driver to integrated panels. Panel 0
selects Elecrow BUSY-based revision detection and defaults to rotation 2;
panels 1, 2, and 3 select the legacy Elecrow, green-sticker Elecrow, and
Waveshare V2 profiles. Panel 4 selects Elecrow's 792x272 cascaded dual-SSD1683
profile and defaults to rotation 0.

Use the module's eight-wire SPI connector and power it from the same 3.3 V
logic domain as the ESP32. The module keeps its last image after detach.
If SolarOS creates a display shell on `epd0`, run `sessions`, close that session
with `session close <id>`, and then detach the expansion.

An M5Stack Unit CardKB attaches at its fixed I2C address and becomes a shared
keyboard source for the shell and foreground apps:

```text
expansion attach cardkb cardkb0 i2c=i2c0 addr=0x5f
input test cardkb0
expansion detach cardkb0
```

The CardKB firmware produces characters after key release. SolarOS maps its
four navigation values to the same logical arrow keys used by PS/2 and BLE
keyboards. The module's Fn combinations are device-specific and are ignored.

RTC and environmental sensor modules use the same named-I2C lifecycle.
Environmental adapters register named providers with the generic sensor
service. A provider can supply temperature, humidity, or both, and multiple
providers can be active at the same time:

```text
expansion attach pcf85063 rtc0 i2c=i2c0 addr=0x51
expansion attach shtc3 environment0 i2c=i2c0 addr=0x70
date
temperature
humidity
temperature list
humidity list
expansion detach environment0
expansion detach rtc0
```

The PCF85063 `irq=<gpio>` binding is optional. The Waveshare board profile
reserves its routed RTC interrupt on GPIO15; external modules can omit `irq`
when only clock and calendar access is required.
The generic RTC service discovers alarm, countdown, and interrupt-status
support from the attached chip adapter, so applications do not depend on the
PCF85063 register interface.
The generic sensor service similarly exposes provider names, driver names, and
temperature/humidity capabilities without exposing a chip register interface.

`battery-adc` takes an ADC pin and a divider ratio in thousandths. For a 2:1
resistive divider, use `divider=2000`:

```text
expansion attach battery-adc battery0 adc=gpio4 divider=2000
battery
expansion detach battery0
```

Use a high-impedance divider suitable for the expected battery voltage. The
divided voltage must remain inside the ESP32 ADC input range.

Other input devices follow the same lifecycle:

```text
expansion attach gpio-keys keys0 key:UP=gpio17 key:ENTER=gpio2
expansion attach rotary-encoder wheel0 a=gpio17 b=gpio2
expansion bus create ps2 ps2mouse clock=gpio17 data=gpio18
expansion attach ps2-mouse mouse0 ps2=ps2mouse
expansion attach analog-joystick joystick0 x=adc2 y=adc4 min=0 center=1650 max=3300 deadzone=100
input status
```

Use only bindings listed by `expansion drivers` and resources shown on the
running board. A rotary encoder decodes interrupts from its quadrature A/B
signals and publishes Up/Down detents; wire its independent push switch through
`gpio-keys`. A PS/2 mouse publishes relative pointer events. An analog joystick
consumes two scalar streams and publishes axes, never keys.
Foreground Python and Lua applications receive those pointer, axis, and gesture events
through `solaros.input`; use `solaros.tui.getch()` for keyboard characters.

On a board without built-in SD hardware, an SPI microSD adapter can provide
removable storage. The SPI bus must include MISO and declare the selected CS
pin. Attaching mounts the detected FAT volume at `/sdcard`. Unmount it before
detaching the adapter:

```text
expansion attach sdspi card0 spi=spi0 cs=gpio4
disk lsblk
disk umount
expansion detach card0
```

If the adapter has a switched supply, add `power=<gpio>` or
`power=<controller:line>`. The SD driver enables that line before probing and
disables it after unmount and detach. The SPI bus and CS remain native ESP
peripheral resources.

The `sdspi` attach command prints the card probe result to the invoking shell.
The report includes the card identity, type, negotiated speed, capacity, CSD/SSR
details, mount point, and any underlying block-I/O or FatFs mount error.

An SDMMC adapter uses direct clock, command, and data bindings. On ESP32-S3 the
signals can use the GPIO matrix. Classic ESP32 accepts only the native slot-1
pinout: CLK GPIO14, CMD GPIO15, D0 GPIO2, and optionally D1 GPIO4, D2 GPIO12,
D3 GPIO13.

```text
expansion attach sdmmc card0 clk=gpio14 cmd=gpio15 d0=gpio2
disk lsblk
disk umount
expansion detach card0
```

An RFM95W wired to the ESP32-S3-DevKitC-1 `spi0` bus with NSS on GPIO4 and
reset on GPIO5 attaches as a multimode packet radio:

```text
expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
radio status radio0
radio profile apply radio0 lora-eu868
```

Connect an antenna suitable for the module band before transmitting. See the
expansion reference for the complete wiring and modulation configuration.

Use `rfm69` for the 13 dBm RFM69W/CW modules and `rfm69h` for the 20 dBm
RFM69HW/HCW variants. For example:

```text
expansion attach rfm69h radio0 spi=spi0 cs=gpio4 reset=gpio5
radio config radio0 power 20
```

Both variants are 3.3 V devices. High-power transmission requires a supply that
can sustain the module's transmit-current peak and an antenna appropriate for
the selected band.

A WS2812/NeoPixel strip uses one runtime-safe GPIO and a declared pixel count:

```text
expansion attach neopixel pixels0 data=gpio1 count=8
neopixel fill pixels0 16 0 0
neopixel set pixels0 3 0 16 0
neopixel clear pixels0
expansion detach pixels0
```

SolarOS stores colors in RGB form and transmits the strip's standard GRB wire
order. Attach clears all declared pixels. `set` and `fill` refresh immediately
in the shell; scripting APIs buffer changes until `show()`.

An LEDC PWM audio output uses one runtime-safe PWM pin and appears as a normal
mono playback device:

```text
expansion attach audio-pwm pwm0 pwm=gpio1
audio devices
audio default pwm0
aplay /audio/example.mp3
audio default auto
expansion detach pwm0
```

The output is 8-bit PWM with a 78.125 kHz carrier and a 16 kHz native PCM rate.
It is a signal output, not a speaker driver. Put a reconstruction low-pass
filter, DC-blocking/coupling stage, and suitable amplifier between the GPIO and
a speaker. A bare speaker can overload and damage the GPIO. Only one LEDC PWM
audio device can be attached at a time.

A PCM5102A module uses three runtime-safe GPIOs and appears as a normal stereo
playback device:

```text
expansion attach pcm5102 dac0 i2s=i2s1 bck=gpio1 din=gpio2 rck=gpio3
audio devices
audio default dac0
aplay /audio/example.mp3
audio default auto
expansion detach dac0
```

The example pins are valid on the Waveshare board. Connect the module's SCK pin
to ground; the driver emits 64 BCK cycles per stereo frame so the PCM5102A can
derive its system clock with the internal PLL. The driver registers
`dac0.playback` as an exclusive 16 kHz, 16-bit PCM sink. Mono input is
duplicated to both channels and device volume is applied in software. Select a
runtime-safe controller explicitly with the `i2s=` binding, leaving other I2S
controllers available to onboard audio or composite video.
PCM5102A modules provide line-level output; connect an amplifier or powered
input rather than a passive speaker.

A PCM1808 module uses four runtime-safe GPIOs and appears as a stereo capture
device:

```text
expansion attach pcm1808 adc0 mclk=gpio1 bck=gpio2 ws=gpio3 dout=gpio17
audio devices
arecord -d 5 -i adc0.capture /sdcard/pcm1808.wav
expansion detach adc0
```

The example consumes every runtime-safe GPIO on the Waveshare expansion
header. Before applying power, configure the module for slave I2S mode:
`MD1=0`, `MD0=0`, and `FMT=0`. Connect `MCLK` to the module's `SCKI` or `SCK`
pin, `WS` to `LRCK`, and `DOUT` to `DOUT`. The driver supplies a 4.096 MHz
system clock and 64 BCK cycles per 16 kHz stereo frame, receives the PCM1808's
24-bit samples, and registers `adc0.capture` as an exclusive signed 16-bit PCM
source. Use the module-rated supply and a common ground; raw PCM1808 circuits
need separate analog and digital supplies as specified by the manufacturer.
On modules that expose both rails, connect `+5V` for the analog supply and
`3.3V` for the digital supply; powering only `3.3V` leaves the converter's
analog section unpowered.

The integrated codec and classic ESP32 DAC backends are also attachable when
the target MCU and board resources support them. Codec attachments need a
named I2C bus, an I2S controller, and all six audio GPIO signals:

```text
expansion attach es8311-es7210 audio0 i2c=i2c0 i2s=i2s0 mclk=gpio38 bck=gpio14 ws=gpio13 din=gpio12 dout=gpio45 pa=gpio46
expansion attach es8311-duplex audio0 i2c=i2c0 i2s=i2s0 mclk=gpio42 bck=gpio10 ws=gpio11 din=gpio12 dout=gpio9 pa=gpio46
```

`es8311-es7210` and `es8311-duplex` are ESP32-S3 drivers. `esp32-dac` is for the
classic ESP32 internal DAC on GPIO25 or GPIO26. Its optional `neg` binding
enables differential output, and `amp` plus `active=0|1` controls an amplifier
enable pin:

```text
expansion attach esp32-dac audio0 pos=gpio26 amp=gpio25 active=1
```

Only one of these primary audio backends can be attached at a time. A fixed
board-default `audio0` cannot be detached.

The integrated display controller drivers can also create auxiliary display
targets. Use a name other than the reserved primary name `display0` for a
runtime attachment. Each controller driver supports one attached instance, so
its board-integrated `display0` and an auxiliary instance cannot coexist:

```text
expansion attach st7305 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5
expansion attach ili9341 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5 bl=gpio6 pwm=1 active=1
expansion attach st7796 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5 bl=gpio6 pwm=1 active=1
display test lcd0
expansion detach lcd0
```

`st7305`, `st7796`, and `ili9341` are available on ESP32 and ESP32-S3 firmware.
The I2S-based `cvbs-pal` and `vga32` drivers are available on classic ESP32.
Composite PAL uses I2S0 and GPIO25. VGA32 uses I2S1 plus its six RGB and two
sync GPIOs; see the expansion reference for its full binding list. A fixed
board-default `display0` cannot be detached.

## Quick reference

solaros.expansion.drivers() lists compiled drivers and devices() lists
currently attached devices with normalized bindings. attach(driver, name,
bindings) and detach(name) manage them. Never assume an example name such as
lcd0 or oled0 exists; inspect devices() or use a name explicitly supplied by
the user. Foreground scripts consume attached pointer, axis, and gesture sources through
solaros.input.
