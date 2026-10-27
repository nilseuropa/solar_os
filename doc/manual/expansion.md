+++
id = "expansion"
title = "Expansion drivers and attached devices"
section = "hardware"
summary = "Discover, attach, and detach package-gated expansion devices"
aliases = ["devices", "drivers", "expansion-export", "ssd1683", "epaper", "e-paper", "st7305", "ili9341", "st7796", "st7789", "cvbs", "pal", "vga32", "cardkb", "tdeck-keyboard", "tab5-keyboard", "keyboard", "tca8418", "inputronic-keyboard", "lilygo-pager-keyboard", "rotary-encoder", "ft6336", "gt911", "mgc3130", "skywriter", "sdmmc", "sdspi", "micro-sd", "pcf85063", "ds3231", "shtc3", "battery-adc", "max17048", "audio-pwm", "ledc-audio", "pcm1808", "i2s-adc", "pcm5102", "pcm5102a", "i2s-output", "i2s-dac", "es8311", "es7210", "esp32-dac", "rfm69", "rfm69h", "rfm95", "sx1262", "lr11xx", "lr1110", "lr1120", "lr1121", "ublox-mia-m10q", "st25r3916", "bhi260ap", "drv2605", "bq25896", "bq27220", "xl9555", "neopixel", "ws2812", "lora", "fsk", "gfsk", "msk", "gmsk", "ook"]
keywords = "python lua expansion device driver category attach detach save startup export manifest snapshot custom board bindings display epaper e-paper ssd1677 ssd1683 st7305 ili9341 st7796 st7789 cvbs pal composite vga vga32 waveshare cardkb m5stack tab5-keyboard tdeck keyboard tca8418 inputronic lilygo-pager-keyboard keymap rotary encoder quadrature mouse joystick pointer ft6336 gt911 mgc3130 skywriter 3d gesture airwheel input i2c sd sdmmc sdspi microsd storage oled lcd rtc pcf85063 ds3231 sensor shtc3 peripheral battery battery-adc fuel gauge max17048 bq27220 charger bq25896 axp2101 audio pwm ledc pcm1808 adc pcm5102 i2s-output es8311 es7210 esp32 dac i2s radio rfm69 rfm69h rfm95 sx1262 lr11xx lr1110 lr1120 lr1121 modem sim7670 gnss ublox mia-m10q nfc st25r3916 imu bhi260ap qmi8658 haptic drv2605 gpio expander xl9555 neopixel ws2812 rgb led strip fsk gfsk msk gmsk ook lora"
packages_any = ["service_expansion"]
+++
# Expansion drivers and attached devices

Expansion drivers let you add displays, keyboards, sensors, audio devices,
and other hardware while SolarOS is running. A driver can serve both built-in
and added hardware. Its availability depends on the board and firmware.

Built-in devices attach at boot and appear in `expansion devices` with board
origin. Most are fixed; a board manifest can explicitly make an individual
device detachable. Use `expansion detach <name>` to release it and
`expansion attach <name>` to restore its board bindings. Detachment lasts until
restoration or reboot and does not imply that the hardware is powered off.
Runtime attachments use named buses and
available pins; detach them when finished to release those resources.

Digital control lines, such as a device's power switch, may use an ESP32 GPIO
or a GPIO-expander line such as `gpiox0:12`. Bus signals, PWM, interrupts, and
other peripheral signals still require suitable native ESP32 pins.

## Discover what is present

```text
expansion
expansion status
expansion drivers
expansion devices
display list
```

`expansion` opens the device manager. Its Devices view lists current
attachments and opens their details; detachable devices can be removed
after confirmation. Its Drivers view groups drivers by category and opens an
attachment form for drivers supported by the running board. Binding forms use
existing named buses; create, attach, detach, or remove buses in the `io` app.
Press Enter on a field to choose or edit its value. While editing the device
name, Enter accepts the name and returns to the attachment menu.
Display panel choices list their native resolutions and module names. Optional
panel fields include a default choice. Rotation has its own selection list with
degree labels and only the orientations supported by the driver. Leave its
default choice selected to use the driver's default rotation.
`expansion status` retains the textual capabilities, buses, devices, and claims
report for scripts and terminal inspection.

`expansion drivers` lists the included drivers by category. `expansion devices`
shows attached instances, their wiring, readiness, and whether they start at
boot.

From a script, inspect `solaros.expansion.drivers()` and
`solaros.expansion.devices()`. A driver existing in firmware does not mean a
physical device is attached.

## Attach deliberately

Run `expansion attach <driver>` to list that driver's required and optional
bindings. After entering a driver and device name, Tab completion offers its
bindings and values, including display `panel` and `rotation` options.

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

The file records runtime buses and driver attachments, plus the base board
identity. It omits secrets, service state, and startup commands. Attachments
using the `manual` resource profile cannot be exported.

Copy it to a SolarOS source tree and run:

```sh
python3 scripts/board_config.py --expansion-manifest my-hardware.toml
```

The configurator asks for a new board identity and checks the combined wiring.
After you build and flash that target, the imported devices attach at boot.
Remove matching temporary startup commands yourself. See
[Promoting a runtime configuration](boards.md#promoting-a-tested-runtime-configuration)
for the manifest format and build steps.

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

### DS3231 real-time clock

Use a named I2C bus running at 100 or 400 kHz. Connect SDA, SCL, ground, and
3.3 V power. For alarm interrupts, also connect INT/SQW to an available ESP32
GPIO with a pull-up to 3.3 V, and add `irq=<pin>` to the attachment command.

```text
expansion attach ds3231 rtc0 i2c=i2c0 addr=0x68
rtc status
date
ntp
rtc alarm set 07:30:15
rtc pending
rtc ack alarm
rtc alarm clear
expansion detach rtc0
```

Only one RTC/time provider can be attached. A board with a fixed onboard RTC
already occupies that slot. The `ds3231` group enables this driver; the `full`
flavor includes it on boards with I2C.

The calendar uses 2000–2099 with the century bit cleared and Sunday numbered
as zero in SolarOS. Oscillator-stop status leaves the clock unreliable until
time is set, for example with `ntp` on a connected device. Existing
12-hour clocks are read correctly; setting time writes 24-hour format.

Alarm 1 supports seconds, seconds and minutes, time of day, or time of day
with either a date of the month or weekday. Other match combinations are
unsupported. Alarm 2 interrupts are disabled, and `rtc timer` is unsupported.
The existing Python and Lua time/RTC APIs use this provider too. The alarm
interrupt is available to the existing sleep policy when the selected GPIO
supports waking the ESP32.

### Other I2C sensors

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
Use `rtc` to inspect the attached clock's alarm and timer support. Use
`temperature list` and `humidity list` to find available sensor providers.

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

GT911 attachments can additionally bind `reset=gpioN`, `power=gpioN`, and
`active=0|1` for reset/address selection and touch-rail control. The default
power active level is high. Optional `home_key=<numeric-key>` maps the
capacitive Home pad to a SolarOS key tap on release; zero disables it. Touch
state is retained until the controller provides a fresh frame.

An SDMMC adapter uses direct clock, command, and data bindings. Optional
`power=gpioN active=0|1` bindings control a slot rail, with an 80 ms off / 120 ms
on cycle before each card initialization. The default active level is high.
On ESP32-S3 the signals can use the GPIO matrix. Classic ESP32 accepts only the native slot-1
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

## Display Link Protocol over streams

`stream-dlp` connects a DLP peripheral through an existing exclusive duplex
byte stream. It registers the supplied name as a display target for text,
graphics, and foreground applications. The `stream_dlp` firmware group is
included in the `full` flavor, including boards without a built-in display.
The stream owns transport setup; the driver owns DLP discovery, format
selection, uploads, presentation completion, and recovery.

```text
stream tcp connect pico0 192.0.2.10 7777
expansion attach stream-dlp lcd1 stream=pico0
display test lcd1
expansion detach lcd1
stream tcp remove pico0
```

Replace the address with a reachable DLP endpoint. For UART, create a UART
port with the required pins and fixed baud first, then select its byte-stream
ID. For example, on approved free pins and an unused controller:

```text
expansion bus create uart dlp-uart port=uart1 tx=<host-tx-gpio> rx=<host-rx-gpio> baud=921600
expansion attach stream-dlp lcd1 stream=dlp-uart
```

Cross host TX to peripheral RX and host RX to peripheral TX, with common
ground. Radio-backed streams follow the same contract. There is no baud
negotiation or UART READY signal. Optional hardware reset is separate from
the stream driver.

`stream=` completes compatible registered streams. Attachment claims the
stream exclusively; it remains claimed until detach. Stop applications and
release display claims before detaching. Detach frees the local DLP image,
conversion and protocol buffers and releases the stream claim. The stream
itself remains registered; a Link stream's RX/TX queues are released by
`link stream remove <port>`.

`timeout-ms=100..60000` sets discovery
and the minimum command timeout, default 5000 ms. Peripheral presentation
limits may require longer waits. Discovery retries transient stream failures
within its connection deadline. A failed presentation returns an error; the
next presentation reopens the stream, discovers a fresh protocol session,
and uploads a full frame. Dimensions and supported formats must remain fixed
for the attachment; detach and attach again after changing the peripheral.

Both DLP drivers use the same host core, including negotiated window uploads,
compression, retained-state checks, and changed-region uploads. Private
[Python](python.gfx.md#direct-dlp-connections) and
[Lua](lua.gfx.md#direct-dlp-connections) connections use that core without
registering a named display.

Add `compressed=1` to either DLP attachment to enable negotiated lossless LZ4
compression. For example:

```text
expansion attach stream-dlp lcd1 stream=pico0 compressed=1
expansion attach spi-dlp lcd2 spi=<bus> cs=<allowed-cs-gpio> compressed=1
```

The setting belongs to that attachment and is preserved when you save it for
startup. Omitted or `compressed=0` retains the existing raw/RLE behavior.
With the option enabled, the sender chooses raw, RLE, or LZ4 per block;
unsupported endpoints fall back to raw/RLE. Compression preserves every pixel
and works with all negotiated pixel formats. The receiver needs no separate
compression switch.

## Display Link Protocol over SPI

`spi-dlp` attaches a Display Link Protocol (DLP) peripheral under the supplied display name.
The peripheral owns its physical panel. SolarOS reads its native dimensions
and accepted pixel formats, selects a transfer format, and registers a display
target for text, graphics, and foreground applications. A receive-capable SPI
bus with MISO is required. The `spi_dlp` firmware group supplies the driver
and is selected by the `full` flavor on compatible boards.

```text
expansion attach spi-dlp lcd1 spi=<bus> cs=<allowed-cs-gpio>
expansion attach spi-dlp lcd1 spi=<bus> cs=<allowed-cs-gpio> ready=<allowed-input-gpio> reset=<allowed-output-gpio>
```

READY and RESET are independently optional. Without READY, transfers use
polling and the peripheral's minimum spacing. RESET targets the peripheral
controller, and defaults to active low with a 10 ms pulse and a 5 s boot
deadline. Optional parameters are `clock=<kHz>`, `reset-ms=<ms>`,
`boot-ms=<ms>`, and `reset-active=0|1`. The clock ceiling defaults to 10000 kHz
and is limited by the peripheral's reported maximum. Width and height are
discovered rather than supplied as bindings.

The host accepts dimensions up to 2040 pixels per axis and 1,048,576 total
pixels, subject to available memory. It keeps a monochrome text buffer, an
RGB565 composition buffer, and one conversion row. When memory permits, two
additional native-format buffers (each at most 256 KiB) track changes. The
driver uploads changed rectangles, uses optional byte RLE when useful, and
checks retained state before skipping an unchanged frame. It sends a full
replacement after content loss or when partial updates cost more. Larger
images and cache allocation failures use bounded full-frame uploads. Color
and indexed graphics are converted to the negotiated format. Changing geometry requires detaching
and recreating the target after releasing display claims.

The DLP SPI profile retains SDLP v1 mode 1 and 256-byte full-duplex cells.
The portable protocol also supports ordered byte streams; this driver uses
SPI. Its negotiated format
describes wire pixels; it does not require any particular MCU or panel bus.
See the [driver reference](expansion.reference.md#spi-display-link) for bindings.

## Quick reference

solaros.expansion.drivers() lists registered drivers and devices() lists
currently attached devices with normalized bindings. attach(driver, name,
bindings) and detach(name) manage them. Never assume an example name such as
lcd0 or oled0 exists; inspect devices() or use a name explicitly supplied by
the user. Foreground scripts consume attached pointer, axis, and gesture sources through
solaros.input.

`spi-dlp` attaches a DLP endpoint using `spi` and `cs`, with independent
optional `ready` and `reset` GPIOs. Native dimensions and wire formats are
discovered during attachment. Optional `clock` is a ceiling in kHz;
`reset-ms`, `boot-ms`, and `reset-active` configure reset and discovery.

`stream-dlp` attaches a DLP endpoint through `stream=<existing-exclusive-duplex-byte-stream>`.
Optional `timeout-ms=100..60000` defaults to 5000. Configure UART, TCP, BLE, or radio
through its stream provider first; there is no baud negotiation. Attachment
claims the stream until detach; release display claims before detaching.
`stream tcp connect <name> <host> <port>` creates an outbound TCP stream.
`stream ble connect <name> <address> <address-type>` creates an outbound native
CoC stream. A `stream ble listen <name>` endpoint can also host a DLP client;
BLE central/peripheral roles are independent of DLP direction.
