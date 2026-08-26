+++
id = "expansion"
title = "Expansion drivers and attached devices"
section = "hardware"
summary = "Discover, attach, and detach package-gated expansion devices"
aliases = ["devices", "drivers", "ssd1683", "epaper", "e-paper", "cardkb", "keyboard", "sdspi", "micro-sd", "audio-pwm", "ledc-audio", "pcm5102", "pcm5102a", "i2s-dac", "rfm69", "rfm69h", "rfm95", "neopixel", "ws2812", "lora", "fsk", "gfsk", "msk", "gmsk", "ook"]
keywords = "python lua expansion device driver attach detach bindings display epaper e-paper ssd1683 waveshare cardkb m5stack keyboard mouse joystick pointer input i2c sd sdspi microsd storage oled lcd sensor peripheral audio pwm ledc pcm5102 i2s dac radio rfm69 rfm69h rfm95 neopixel ws2812 rgb led strip fsk gfsk msk gmsk ook lora"
packages_any = ["service_expansion"]
+++
# Expansion drivers and attached devices

Expansion drivers turn named buses and safe GPIO slots into active displays,
radios, sensors, or manual resource profiles. Drivers are package-gated, so the
available list depends on the firmware and board.

Integrated hardware uses the same composition model. A board profile declares
its fixed buses and default attachments, which are created at boot and shown by
`expansion devices`. For example, Freenove `touch0` is an `ft6336` attachment,
and TTGO VGA32 `keyboard0` is a `ps2-keyboard` attachment. The `input` command
then presents their semantic keyboard, pointer, or axis behavior.

Named MIDI connections are created as buses rather than attached drivers. Use
`expansion bus create midi <name> tx=<gpio> rx=<gpio>`; SolarOS chooses the UART
backend and the `midi` background job owns the connection while it runs.

## Discover what is present

```text
expansion drivers
expansion devices
display list
```

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

A Waveshare 4.2-inch V2 monochrome e-paper module uses the SSD1683 expansion
driver and registers a 400x300 display target:

```text
expansion attach ssd1683 epd0 spi=spi0 cs=gpio10 dc=gpio17 reset=gpio16 busy=gpio15
display test epd0
display mode epd0 refresh=fast
expansion detach epd0
```

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

Other input devices follow the same lifecycle:

```text
expansion attach gpio-keys keys0 key:UP=gpio17 key:ENTER=gpio2
expansion bus create ps2 ps2mouse clock=gpio17 data=gpio18
expansion attach ps2-mouse mouse0 ps2=ps2mouse
expansion attach analog-joystick joystick0 x=adc2 y=adc4 min=0 center=1650 max=3300 deadzone=100
input status
```

Use only bindings listed by `expansion drivers` and resources shown on the
running board. A PS/2 mouse publishes relative pointer events. An analog
joystick consumes two scalar streams and publishes axes, never keys.

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

The `sdspi` attach command prints the card probe result to the invoking shell.
The report includes the card identity, type, negotiated speed, capacity, CSD/SSR
details, mount point, and any underlying block-I/O or FatFs mount error.

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
expansion attach pcm5102 dac0 bck=gpio1 din=gpio2 rck=gpio3
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
duplicated to both channels and device volume is applied in software. Current
dual-I2S boards use I2S1, leaving onboard audio or composite video on I2S0.
PCM5102A modules provide line-level output; connect an amplifier or powered
input rather than a passive speaker.

## Quick reference

solaros.expansion.drivers() lists compiled drivers and devices() lists
currently attached devices with normalized bindings. attach(driver, name,
bindings) and detach(name) manage them. Never assume an example name such as
lcd0 or oled0 exists; inspect devices() or use a name explicitly supplied by
the user.
