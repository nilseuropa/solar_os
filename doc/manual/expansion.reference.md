+++
id = "expansion.reference"
title = "Expansion hardware reference"
section = "hardware"
summary = "Resource rules, workflows, drivers, bindings, and wiring examples"
aliases = ["hardware.expansion"]
keywords = "expansion ports gpio adc pwm ledc audio pcm1808 buses i2c spi uart midi wiring displays neopixel ws2812 rgb led strip"
packages_any = []
+++
# Expansion Ports

SolarOS treats an expansion port as a board-described collection of resources,
not as one fixed connector standard. A board may expose individual GPIO pins,
named I2C, SPI, UART, or MIDI buses, or free pins that can be routed to an approved
spare peripheral host at runtime.

Use `expansion layout` for the physical connector arrangement, and use
`expansion status` and `gpio list` on the running device for the authoritative
resource view. The layout overlays live pin policy and claims; the available
resources depend on the board and the compiled firmware flavor. Boards with
multiple named headers can be filtered, for example with `expansion layout J1`.

## Resource Model

| Term | Meaning | Ownership and lifetime |
| --- | --- | --- |
| Connector pin | A signal physically present on an expansion header or breakout. Physical presence does not make a pin safe for runtime control. | Described by the board profile. |
| Runtime GPIO | A connector pin approved for direct `gpio` and 1-Wire use, and for `adc` or `pwm` where the board tables allow it. | Claimed while a service or attached device uses it. |
| Board-defined bus | A named bus with fixed pins, such as `i2c0` or `spi0`. | Registered at boot and cannot be removed. A named UART can still be detached and reattached. |
| Runtime bus | A named bus routed onto approved free pins and a spare hardware host. | It can be removed when idle. UART controller and pin claims follow attach/detach; other bus signals remain claimed for the descriptor lifetime. |
| Expansion driver | Code that knows how to initialize and operate a supported external device. | Listed by `expansion drivers`; availability is package- and capability-filtered. |
| Attached device | A named driver instance bound to buses, addresses, chip-selects, or GPIO roles. | Acquires resource leases on attach and releases them on detach. |

Board pin policy has three levels:

| Policy | Direct GPIO | Runtime bus routing | Typical use |
| --- | --- | --- | --- |
| Free | Yes | Yes | Uncommitted expansion pin. |
| Releasable | No | Yes, after its current service releases it. | UART or another default board role. |
| Fixed | No | No | Boot straps, flash/PSRAM, display, storage, USB, controls, or other board hardware. |

This policy is separate from physical connector membership. For example, a
strapping pin may appear on a header and in the physical connector description
while remaining blocked from runtime use.

## Board Resources

### GPIO, ADC, and PWM

| Board | Physical expansion signals | Runtime GPIO and PWM | Runtime ADC | Connector restrictions |
| --- | --- | --- | --- | --- |
| SolarTerm (Waveshare ESP32-S3-RLCD-4.2) | GPIO0-GPIO3, GPIO13, GPIO14, GPIO17-GPIO20, GPIO43, GPIO44 | GPIO1-GPIO3, GPIO17 | GPIO1-GPIO3, GPIO17 | GPIO0 is BOOT; GPIO13/GPIO14 are I2C; GPIO18 is KEY; GPIO19/GPIO20 are native USB; GPIO43/GPIO44 belong to `uart0` by default. |
| ESP32-S3 Display 4.0-inch (FNK0104S) | GPIO2, GPIO3, GPIO14-GPIO16, GPIO21, GPIO43, GPIO44 | GPIO2, GPIO3, GPIO14, GPIO21 | GPIO2, GPIO3, GPIO14 | GPIO15/GPIO16 are shared I2C; GPIO43/GPIO44 belong to `uart0`; GPIO4 is fixed audio MCLK, not a connector GPIO. |
| ES3C28P 2.8-inch ESP32-S3 Touch Display | GPIO2, GPIO3, GPIO14-GPIO16, GPIO21, GPIO43, GPIO44 | GPIO2, GPIO3, GPIO14, GPIO21 | GPIO2, GPIO3, GPIO14 | GPIO15/GPIO16 are shared by touch, audio control, and the I2C connector; GPIO17/GPIO18 belong to touch; GPIO43/GPIO44 belong to `uart0`. |
| ES3N28P 2.8-inch ESP32-S3 Display | GPIO2, GPIO3, GPIO14-GPIO16, GPIO21, GPIO43, GPIO44 | GPIO2, GPIO3, GPIO14, GPIO21 | GPIO2, GPIO3, GPIO14 | GPIO15/GPIO16 are shared I2C; GPIO43/GPIO44 belong to `uart0`; GPIO17/GPIO18 are reserved for the unpopulated touch footprint. |
| Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | GPIO3, GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO20 | GPIO3 is physically exposed but blocked as a strapping pin. |
| Elecrow CrowPanel ESP32-S3 5.79-inch E-paper | GPIO3, GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO20 | GPIO3 is physically exposed but blocked as a strapping pin. |
| Waveshare ESP32-S3-ePaper-3.97 | GPIO41-GPIO44 on the I2C/UART pads | None | None | GPIO41/GPIO42 belong to shared `i2c0`; GPIO43/GPIO44 belong to `uart0`. The board exposes no unmanaged general-purpose signal. |
| CL-32 | GPIO1-GPIO4, GPIO8, GPIO15-GPIO21, GPIO35-GPIO44, GPIO46-GPIO48 | GPIO4, GPIO8, GPIO15-GPIO18, GPIO21, GPIO38-GPIO42, GPIO47, GPIO48 | GPIO4, GPIO8, GPIO15-GPIO18 | GPIO1/GPIO2 are shared I2C; GPIO3 is the shared peripheral interrupt; GPIO19/GPIO20 are USB; GPIO35-GPIO37 are Octal PSRAM; GPIO43/GPIO44 belong to `uart0`; GPIO46 is an input-only strapping signal. EX1 uses a proprietary M.2 pinout. |
| T-LoRa-Pager | GPIO2, GPIO3, GPIO9, GPIO33-GPIO35, GPIO43, GPIO44, and CE on XL9555 P1.1 (`gpiox0:9`) | GPIO9 | GPIO9 | GPIO2/GPIO3 are shared I2C; GPIO33-GPIO35 are shared SPI; GPIO43/GPIO44 belong to releasable `uart0`; CE is an expanded digital control line, not ESP32 GPIO9. |
| T-Deck Plus | None; the declared connector view contains internal peripheral wiring only | None | None | The board has no general-purpose expansion connector. All declared ESP32 pins belong to fixed display, input, storage, audio, radio, GNSS, battery, USB, or bus resources. |
| ThinkNode M9 | None; the declared connector view contains internal peripheral wiring only | None | None | The board has no general-purpose expansion connector. All declared ESP32 pins belong to fixed display, input, storage, radio, GNSS, RTC, battery, buzzer, power, or bus resources. |
| ESP32-S3-DevKitC-1-N16R8 | ESP32-S3 signals broken out on the DevKitC headers | GPIO1, GPIO2, GPIO4-GPIO7, GPIO10, GPIO14-GPIO18, GPIO21, GPIO39-GPIO42, GPIO47 | GPIO1, GPIO2, GPIO4-GPIO7, GPIO10, GPIO14-GPIO18 | GPIO0 is BOOT/KEY; GPIO3/GPIO45/GPIO46 are other strapping pins; GPIO19/GPIO20 are native USB; GPIO35-GPIO37 are Octal PSRAM; GPIO38/GPIO48 are reserved for either RGB LED revision; GPIO43/GPIO44 are `uart0`. |
| GOOUUU ESP32-S3CAM | ESP32-S3 signals broken out on the 2x20 side headers | GPIO1, GPIO14, GPIO21, GPIO41, GPIO42, GPIO47 | GPIO1, GPIO14 | GPIO0 is BOOT/KEY; GPIO2 is reserved for the unverified status LED; GPIO3/GPIO45/GPIO46 are strapping or onboard-button signals; GPIO4-GPIO13 and GPIO15-GPIO18 belong to the camera; GPIO19/GPIO20 are native USB; GPIO35-GPIO37 are Octal PSRAM; GPIO38-GPIO40 are SDMMC; GPIO43/GPIO44 are `uart0`; GPIO48 drives the onboard WS2812. |
| ESP32-S3 DevKitC-1 E-paper Workbench | ESP32-S3 signals broken out on the DevKitC headers | GPIO6, GPIO7, GPIO14, GPIO18, GPIO21, GPIO39-GPIO42, GPIO47 | GPIO6, GPIO7, GPIO14, GPIO18 | GPIO1/GPIO2/GPIO4/GPIO5 belong to the fixed storage SPI bus; GPIO10/GPIO15-GPIO17 belong to the fixed e-paper display; the remaining DevKitC restrictions are unchanged. |
| Waveshare ESP32-S3-SIM7670G-4G V2.0 | GPIO0-GPIO21, GPIO39-GPIO46 on the P2/P3 headers | GPIO2, GPIO3, GPIO7-GPIO14, GPIO39-GPIO42 | GPIO2, GPIO3, GPIO7-GPIO14 | Use CAM OFF. GPIO0 is BOOT/KEY; GPIO1 is the battery ADC divider; GPIO4-GPIO6 are SDMMC; GPIO15/GPIO16 are shared I2C; GPIO17/GPIO18 are the modem UART; GPIO19/GPIO20 are USB; GPIO21 is modem power control; GPIO43/GPIO44 are `uart0`; GPIO45/GPIO46 are strapping pins. |
| ODROID-GO | External IO GPIO4 and GPIO15 | GPIO4, GPIO15 | None | Both pins are also the allowed external chip-select slots on the shared VSPI bus. |
| ESP32-WROVER v3.0 | GPIO0-GPIO5, GPIO12-GPIO15, GPIO18, GPIO19, GPIO21-GPIO23, GPIO25-GPIO27, GPIO32-GPIO36, GPIO39 | GPIO4, GPIO5, GPIO13, GPIO18, GPIO19, GPIO21-GPIO23, GPIO26, GPIO27, GPIO32-GPIO36, GPIO39; PWM excludes input-only GPIO34-GPIO36 and GPIO39 | GPIO32-GPIO36, GPIO39 | GPIO0 is BOOT/KEY; GPIO1/GPIO3 are CH340 `uart0`; GPIO2/GPIO14/GPIO15 are SDMMC; GPIO25 is PAL; GPIO5 is a strapping pin; GPIO34-GPIO36/GPIO39 are input-only. |
| TTGO VGA32 v1.4 | GPIO2, GPIO12-GPIO14, GPIO34, GPIO39 | GPIO34, GPIO39 | GPIO34, GPIO39 | GPIO34/GPIO39 are input-only. GPIO2/GPIO12-GPIO14 belong to the onboard v1.4 microSD interface; GPIO25 audio, VGA, and PS/2 signals are fixed board resources. |

Power and ground pins are physical wiring resources and are not managed by the
SolarOS pin-claim system. Check the board schematic and the external module's
voltage and current requirements before connecting it.

### Named and Runtime Buses

| Board | Board-defined buses | Runtime-routable buses | Notes |
| --- | --- | --- | --- |
| SolarTerm (Waveshare ESP32-S3-RLCD-4.2) | `i2c0`: SDA GPIO13, SCL GPIO14; `spi0`: SCK GPIO11, MOSI GPIO12, CS GPIO40; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, or 1-Wire, using approved free pins | `spi0` is the fixed internal display bus; its pins are not expansion pins. |
| ESP32-S3 Display 4.0-inch (FNK0104S) | `i2c0`: SDA GPIO16, SCL GPIO15; `spi0`: SCK GPIO12, MOSI GPIO11, CS GPIO10; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | The I2C connector shares `i2c0` with touch and audio control. `spi0` is the fixed internal LCD bus. |
| ES3C28P 2.8-inch ESP32-S3 Touch Display | `i2c0`: SDA GPIO16, SCL GPIO15; `spi0`: SCK GPIO12, MISO GPIO13, MOSI GPIO11, CS GPIO10; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | The I2C connector shares `i2c0` with FT6336 touch and ES8311 control. `spi0` is the fixed internal LCD bus. |
| ES3N28P 2.8-inch ESP32-S3 Display | `i2c0`: SDA GPIO16, SCL GPIO15; `spi0`: SCK GPIO12, MISO GPIO13, MOSI GPIO11, CS GPIO10; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | The I2C connector shares `i2c0` with ES8311 control. `spi0` is the fixed internal LCD bus. |
| Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | `spi0`: SCK GPIO12, MOSI GPIO11, CS GPIO45; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c0`/`i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, or named 1-Wire, using approved free pins | `spi0` is the fixed internal SSD1683 bus. SPI3 is shared with microSD and is available for a runtime expansion bus only while the SD card is unmounted. |
| Elecrow CrowPanel ESP32-S3 5.79-inch E-paper | `spi0`: SCK GPIO12, MOSI GPIO11, CS GPIO45; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c0`/`i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, or named 1-Wire, using approved free pins | `spi0` is the fixed internal dual-SSD1683 bus. SPI3 is shared with microSD and is available for a runtime expansion bus only while the SD card is unmounted. |
| Waveshare ESP32-S3-ePaper-3.97 | `i2c0`: SDA GPIO41, SCL GPIO42; `spi0`: SCK GPIO11, MOSI GPIO12, CS GPIO10; `uart0`: TX GPIO43, RX GPIO44 | External I2C devices can share `i2c0`; external UART devices can use `uart0` when the console releases it. | `spi0` is the fixed internal SSD1677 bus. All other GPIOs belong to fixed peripherals, native USB, controls, or power-management signals. |
| CL-32 | `i2c0`: SDA GPIO1, SCL GPIO2; `spi0`: SCK GPIO9, MISO GPIO11, MOSI GPIO10, display CS GPIO6, microSD CS GPIO7; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | EX1, CN1, and CN2 expose the shared `i2c0`; EX1 exposes `uart0`. The internal `core0`, ST7305 display, and microSD attach automatically. `core0` supplies `keyboard0` from the AVR event FIFO and `battery0` from its voltage and power-status registers; the display and microSD share `spi0` with separate chip selects. |
| T-LoRa-Pager | `i2c0`: SDA GPIO3, SCL GPIO2; `spi0`: SCK GPIO35, MISO GPIO33, MOSI GPIO34, CS GPIO38/GPIO21/GPIO36/GPIO39/GPIO9; `uart0`: TX GPIO43, RX GPIO44; `gnss-uart`: TX GPIO12, RX GPIO4 | External devices can share `i2c0` or `spi0`; GPIO9 is the free SPI CS slot. `uart0` is releasable. | The extension header exposes the shared buses and console UART. Display, storage, radio, and NFC share `spi0`; GNSS owns its internal UART. There is no complete exposed pin set for another routed bus. |
| T-Deck Plus | `i2c0`: SDA GPIO18, SCL GPIO8; `spi0`: SCK GPIO40, MISO GPIO38, MOSI GPIO41, CS GPIO12/GPIO39/GPIO9; `uart0`: TX GPIO43, RX GPIO44 at 9600 baud | None through an external connector | Display, storage, radio, keyboard, touch, audio, microphone, and GNSS use fixed internal resources. The board has no general-purpose expansion connector. |
| ThinkNode M9 | `i2c0`: SDA GPIO20, SCL GPIO21; `i2c1`: SDA GPIO7, SCL GPIO6; `spi0`: SCK GPIO40, MISO GPIO38, MOSI GPIO47, CS GPIO16/GPIO48/GPIO39; `uart0`: TX GPIO43, RX GPIO44; `uart1`: TX GPIO3, RX GPIO2 at 115200 baud | None through an external connector | Display, storage, keyboard, RTC, battery, buzzer, and GNSS use fixed internal resources. `i2c0` is the keyboard bus and `i2c1` the peripheral bus; `uart0` is the CH340K USB console; `uart1` belongs to the GNSS receiver. The board has no general-purpose expansion connector. |
| ESP32-S3-DevKitC-1-N16R8 | `i2c0`: SDA GPIO8, SCL GPIO9; `spi0`: SCK GPIO12, MISO GPIO13, MOSI GPIO11, CS GPIO4/GPIO10/GPIO5/GPIO6/GPIO7; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, SPI on `spi3`, UART on `uart1`/`uart2`, or 1-Wire, using approved free pins | The board-defined `spi0` is the normal expansion SPI bus. |
| GOOUUU ESP32-S3CAM | `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c0`/`i2c1`, SPI on `spi2`/`spi3`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | `storage0` and `pixels0` attach automatically. Camera SIOD/SIOC remain camera-owned and are not exposed as a general I2C bus. |
| ESP32-S3 DevKitC-1 E-paper Workbench | `i2c0`: SDA GPIO8, SCL GPIO9; `spi0`: SCK GPIO12, MISO GPIO13, MOSI GPIO11, CS GPIO10/GPIO6/GPIO7; `spi1`: SCK GPIO1, MISO GPIO4, MOSI GPIO2, CS GPIO5; `uart0`: TX GPIO43, RX GPIO44 | I2C on `i2c1`, UART on `uart1`/`uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | `keyboard0`, `display0`, and `storage0` attach automatically. Both SPI hosts are assigned to fixed buses. |
| Waveshare ESP32-S3-SIM7670G-4G V2.0 | `i2c0`: SDA GPIO15, SCL GPIO16; `uart0`: TX GPIO43, RX GPIO44; `modem-uart`: TX GPIO18, RX GPIO17 | SPI on `spi2`/`spi3`, UART on `uart2`, I2S on `i2s1`, or 1-Wire, using approved free pins | `storage0`, `battery0`, `pixels0`, and `modem0` attach automatically. The SIM7670 exclusively owns UART1. Runtime SPI enables attachable packet-radio drivers without treating the cellular modem as the only radio. |
| ODROID-GO | `spi0`: SCK GPIO18, MISO GPIO19, MOSI GPIO23, CS GPIO5/GPIO15/GPIO4; `uart0`: TX GPIO1, RX GPIO3 | I2C on `i2c0`/`i2c1`, UART on `uart1`/`uart2`, or named 1-Wire, using approved free pins | GPIO4/GPIO15 can form an I2C bus when unused. Those pins are also external VSPI chip-select slots and cannot be shared with I2C. VSPI is shared with onboard TFT and SD devices. |
| ESP32-WROVER v3.0 | `uart0`: TX GPIO1, RX GPIO3 | I2C, SPI on `spi2`/`spi3`, UART on `uart1`/`uart2`, or 1-Wire, using free output-capable GPIO4, GPIO5, GPIO13, GPIO18, GPIO19, GPIO21-GPIO23, GPIO26, GPIO27, GPIO32, or GPIO33 | The rear SD slot uses the dedicated one-bit SDMMC host. GPIO34-GPIO36 and GPIO39 are available only for input signals and ADC. |
| TTGO VGA32 v1.4 | `spi0`: SCK GPIO14, MISO GPIO2, MOSI GPIO12, CS GPIO13; `uart0`: TX GPIO1, RX GPIO3; `ps2kbd0`: clock GPIO33, data GPIO32; `ps2mouse0`: clock GPIO26, data GPIO27 | None | `keyboard0` attaches automatically; attach `ps2-mouse` to `ps2mouse0` only when a mouse is connected. GPIO25 is the fixed mono audio DAC output. I2S1 and the six RGB plus two sync pins are permanently reserved for VGA DMA scanout. |

I2C and SPI buses accept shared logical leases. UART, MIDI, and registered 1-Wire bus
instances are exclusive. Registered 1-Wire buses appear in expansion status
and can be addressed by name. Bus names are unique across protocols.

I2C, SPI, UART, MIDI, and 1-Wire buses can be created at runtime. Runtime hardware
buses require an unused board-approved controller or host; all signal pins must
be approved by the board's runtime pin policy. Every named UART has an explicit
attached state. Attaching reserves its controller and pins; the hardware driver
still starts lazily on the first consumer claim and stops after the final claim.
Detaching an idle UART releases the controller and pins but preserves its name
and configuration. Runtime UART descriptors may additionally be removed;
board-defined UART descriptors cannot.
The direct numeric form of the `onewire` command remains available without
creating a named expansion bus.

## Typical Workflow

Start by inspecting the live resource map and registered drivers:

```text
expansion layout
expansion status
gpio list
expansion drivers
expansion scan
```

If the device can use a board-defined bus, attach it directly. The device name
is chosen by the user and becomes the lease owner:

```text
expansion attach ssd1306 oled0 i2c=i2c0 addr=0x3c
expansion devices
display test oled0
expansion detach oled0
```

Runtime I2C and 1-Wire buses use the same lifecycle:

```text
expansion bus create i2c i2c1 port=i2c1 sda=gpio14 scl=gpio15 speed=100000
i2c scan i2c1
expansion bus remove i2c1

expansion bus create onewire onewire0 pin=gpio16
onewire scan onewire0
expansion bus remove onewire0

expansion bus create ps2 ps2kbd clock=gpio17 data=gpio18
expansion attach ps2-keyboard keyboard0 ps2=ps2kbd
input test keyboard0
expansion detach keyboard0
expansion bus remove ps2kbd

expansion bus create uart uart1 port=uart1 tx=gpio14 rx=gpio15 baud=115200
uart status uart1
uart write uart1 AT
expansion bus detach uart1
expansion bus attach uart1
expansion bus remove uart1

expansion bus create midi midi0 tx=gpio1 rx=gpio2
job start midi midi0
midi status
midi monitor
midi note-on 1 60 100
midi note-off 1 60
midi stream add 1 74
control create cutoff midi.cc.1.74 0 127
control bind cutoff parameter synth.filter.cutoff pickup=off
job start controls
job stop midi
expansion bus remove midi0
```

MIDI is a user-facing bus type with an automatically selected UART backend.
Its optional `baud=` defaults to 31250; there is no `port=` argument. The
resolved `uartN` appears in status output only to help diagnose controller
allocation. A standard DIN connection requires an optoisolated MIDI IN circuit
and a current-limited MIDI OUT driver. Never connect DIN MIDI pins directly to
ESP32 GPIOs.

An explicitly configured `midi.cc.<channel>.<controller>` scalar stream retains
the latest matching incoming CC value while the MIDI job runs. The controls job
can normalize its `0..127` range and bind it to one or more application
parameters without consuming messages from MIDI subscribers.

On the Waveshare board, `uart0` owns the releasable GPIO43/GPIO44 pair while it
is attached. From a display or other non-`uart0` shell, detach it before reusing
those pins and attach it again after the temporary bus is removed:

```text
expansion bus detach uart0
expansion bus create uart uart1 port=uart1 tx=gpio43 rx=gpio44
expansion bus remove uart1
expansion bus attach uart0
```

Detaching the port that carries the current shell fails as busy, so the shell
cannot disconnect itself accidentally.

On a board with an approved available SPI host, create a bus before attaching the
device. Creating the bus claims its controller, SCLK, MOSI, and optional MISO
immediately. Each `cs=` option declares an allowed chip-select pin but leaves it
available until a device or one-shot transfer selects it. That user claims both
the GPIO and logical chip-select slot, preventing GPIO or SPI users from driving
it concurrently:

```text
expansion bus create spi spi1 host=spi3 sclk=gpio1 mosi=gpio2 miso=gpio3 cs=gpio17
expansion attach rfm69 radio0 spi=spi1 cs=gpio17
expansion detach radio0
expansion bus remove spi1
```

On the Elecrow CrowPanel, run `disk umount` before creating the runtime SPI3 bus.
Remove that bus before using `disk mount` to make SPI3 available to microSD again.

The `spi` command addresses board-defined and runtime buses by name. This makes
the same transfer tools available for `spi0`, `spi1`, or any other registered
SPI bus:

```text
spi status
spi status spi1
spi xfer spi1 gpio17 0 1m 0x9f 0 0 0
spi read spi1 gpio17 0 1m 4 0xff
spi write spi1 gpio17 0 1m 0xaa 0x55
```

The bus name and chip-select are always explicit. Transfers temporarily claim
the selected chip-select and lease the bus, so they fail cleanly when an
attached device already owns that chip-select.

The `i2c` command also accepts a named bus. Omitting it retains the `i2c0`
shortcut used by existing scripts:

```text
i2c status i2c0
i2c scan i2c0
i2c probe i2c0 0x3c
i2c read i2c0 0x50 0x00 8
i2c write i2c0 0x50 0x00 0xaa 0x55
```

Omit `miso` or use `miso=none` for output-only peripherals. A runtime bus can
only use a host and pins approved by the board profile. It cannot take fixed
display, storage, I2C, USB, or strapping pins. A bus cannot be detached or
removed while it has device leases, and board-defined buses can never be
removed. `expansion bus detach` preserves the named descriptor and works for
every runtime bus plus board buses whose owned pins are marked releasable.
Fixed-pin board buses reject detach.

## Drivers and Bindings

Run `expansion drivers` on the device to see the exact registered set.

| Driver | Device | Required bindings | Result after attach |
| --- | --- | --- | --- |
| `manual` | Resource-only profile | Any valid bus, address, chip-select, GPIO, ADC, or PWM bindings | Claims resources without initializing hardware. |
| `axp2101` | AXP2101 PMIC, fuel gauge, and battery charger | `i2c=<bus> addr=0x34`; optional `input_current=<mA> charge_current=<mA> charge_voltage=<mV>` | Registers the board battery provider and `charger0`; also supplies the reusable ALDO3 control used by the Waveshare SSD1677 display. |
| `rfm69` | HopeRF RFM69W/CW packet radio | `spi=<bus> cs=<pin>`; optional `irq=<pin> reset=<pin>` | Registers a packet-radio target with PA0 power from -18 through 13 dBm. |
| `rfm69h` | HopeRF RFM69HW/HCW high-power packet radio | `spi=<bus> cs=<pin>`; optional `irq=<pin> reset=<pin>` | Registers a packet-radio target with PA_BOOST power from -2 through 20 dBm. |
| `rfm95` | HopeRF RFM95W multimode radio | `spi=<bus> cs=<pin>`; optional `irq=<pin> reset=<pin>` | Registers an FSK/GFSK/MSK/GMSK/OOK/LoRa target for the `radio` command. |
| `sx1262` | Semtech SX1262 packet radio | `spi=<bus> cs=<pin> busy=<pin>`; optional `reset=<pin> irq=<pin>` | Registers a LoRa/(G)FSK packet-radio target for the `radio` command. |
| `pcd8544` | 84x48 SPI LCD | `spi=<bus> cs=<pin> dc=<pin> reset=<pin>` | Registers an auxiliary display target. |
| `ssd1683` | SSD1683 monochrome e-paper | `spi=<bus> cs=<pin> dc=<pin> reset=<pin> busy=<pin>`; optional `power=<pin> clock=<khz> rotation=<0..3> panel=<0..4>` | Registers an auxiliary target, or Elecrow's fixed `display0`, with auto, fast, and full refresh modes. Panel 0 auto-detects the 4.2-inch Elecrow revision; 1 is legacy Elecrow, 2 is green-sticker Elecrow, 3 is Waveshare V2, and 4 is Elecrow's 792x272 dual-controller panel. |
| `uc8279` | UC8279 800x480 monochrome e-paper | `spi=<bus> cs=<pin> dc=<pin> reset=<pin> busy=<pin> panel=<1..2>`; optional `power=<pin> clock=<khz> rotation=<0..3>` | Panel 1 selects X4 Pro glass and sets its PLL; panel 2 selects X4 Classic glass and preserves the panel's programmed PLL. Both address 600 gates with the visible rows at gates 120..599 and require programmed OTP/MTP waveforms. Active-low BUSY, automatic full cleanup every 20 changed frames, and selectable partial/full refresh. Controller and panel selection are explicit; UC8279 X3 792x528 and other variants require separate profiles. |
| `x4pro` | Xteink X4 Pro primary display and frontlight | `spi=<bus> cs=<pin> dc=<pin> reset=<pin> busy=<pin> latch=<pin> cool=<pin> warm=<pin>` | Identifies SSD1677, UC8179, or UC8279 once and caches the controller ID in SolarOS NVS. Native 800x480 landscape, 10 MHz SPI, and 25 kHz dual PWM with a fixed 50/50 mix controlled by `setterm brightness`. The primary attachment holds the shared peripheral latch on. |
| `uc8179` | UC8179 800x480 monochrome e-paper | `spi=<bus> cs=<pin> dc=<pin> reset=<pin> busy=<pin> panel=1`; optional `power=<pin> clock=<khz> rotation=<0..3>` | Panel 1 selects Xteink 800x480 glass with 600 addressed gates and programmed OTP waveforms. Active-low BUSY, automatic full cleanup every 20 changed frames, and selectable partial/full refresh. No controller autodetection; other panel geometries and waveforms need their own profile. |
| `ssd1677` | SSD1677 800x480 monochrome e-paper | `spi=<bus> cs=<pin> dc=<pin> reset=<pin> busy=<pin>`; optional `power=<pin> clock=<khz> rotation=<0..3>` or paired `power_i2c=<bus> power_addr=0x34` through an attached AXP2101 service | Registers an auxiliary target, or Waveshare's fixed `display0`, with automatic dirty-window partial refresh plus selectable partial and full modes. Holds the PMIC attachment while using its ALDO3 rail. |
| `ssd1306` | 128x64 I2C OLED | `i2c=<bus> addr=<address>` | Registers an auxiliary display target. |
| `sh1106` | 128x64 I2C OLED with SH1106 addressing | `i2c=<bus> addr=<address>` | Registers an auxiliary display target with the two-column offset. |
| `st7305` | 400x300 or 384x168 reflective LCD | `spi=<bus> cs=<pin> dc=<pin> reset=<pin>`; optional `panel=0|1 rotation=1|3` | ESP32 and ESP32-S3. Panel 0 is the native 300x400 Waveshare panel; panel 1 is the native 168x384 CL-32 panel. Both boards register their panel as fixed `display0`. |
| `ili9341` | 320x240 color TFT | `spi=<bus> cs=<pin> dc=<pin>`; optional `reset=<pin> bl=<pin> active=0|1 pwm=0|1` | ESP32 and ESP32-S3; ODROID-GO registers it as fixed `display0`. |
| `st7796` | 480x320 color TFT | `spi=<bus> cs=<pin> dc=<pin>`; optional `reset=<pin> bl=<pin> active=0|1 pwm=0|1` | ESP32 and ESP32-S3; Freenove registers it as fixed `display0`. |
| `st7789` | 320x240 color TFT | `spi=<bus> cs=<pin> dc=<pin>`; optional `reset=<pin> bl=<pin> power=<pin> active=0|1 pwm=0|1` | ESP32-S3 driver; T-Deck Plus and the ThinkNode M9 register it as fixed `display0` with shared peripheral power and backlight control. A board with an active-low power rail sets `SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL` in its manifest. |
| `cvbs-pal` | 384x288 or 320x200 monochrome PAL composite output | `i2s=i2s0 out=gpio25` | Classic ESP32 driver; ESP32-WROVER v3.0 registers it as fixed `display0`. |
| `vga32` | Build-selected RGB222 VGA output | `r0=<pin> r1=<pin> g0=<pin> g1=<pin> b0=<pin> b1=<pin> hsync=<pin> vsync=<pin>` | Classic ESP32 driver; claims I2S1 and TTGO VGA32 registers it as fixed `display0`. |
| `cardkb` | M5Stack Unit CardKB | `i2c=<bus> addr=0x5f` | Polls released keys into the shared input service for shells and foreground apps. |
| `inputronic-keyboard` | Soldered Inputronic KEYBOARD (SKU 333360) | `i2c=<bus> addr=0x34 [reset=<gpio>] [irq=<gpio>]` | Polls the 8x10 matrix FIFO and publishes press/release events through the shared input service. Optional active-low reset and interrupt pins. |
| `tdeck-keyboard` | LilyGO T-Deck raw-matrix keyboard | `i2c=<bus> addr=0x55` | Polls the raw matrix and publishes keys through the shared input service. The built-in T-Deck attachment uses its board keymap; other boards can attach the same controller to a named I2C bus. |
| `stc8h-keyboard` | Elecrow STC8H companion-MCU keypad | `i2c=<bus> addr=0x6c\|0x6d`; optional `alt_addr=0x6c\|0x6d backlight=<pwm-pin>` | Reads finished key codes from the STC8H scanner and publishes them through the shared input service. The address identifies the ThinkNode M9 board revision; with `alt_addr` declared, the driver probes both. Arrow, enter, and printable codes follow the CardKB map. Keycaps map Home to app exit, Back to escape, Messages/Maps/Pin to F1/F2/F3, and long-press Pin to F4. The scanner cannot emit tab, pipe, backslash, brackets, or several other characters, so the Message key arms a one-shot compose layer for the next key: space or `t` gives tab, `p` gives `\|`, `b` backslash, `e` `=`, `5` `%`, `6` `^`, `9`/`0` `[`/`]`, `(`/`)` `{`/`}`, `,`/`.` `<`/`>`, `g` a backtick, and `n` `~`. Composed arrows navigate instead: up and down page through the terminal scrollback, and right is tab, which the keypad cannot send any other way. Composed left sends a plain left arrow and disarms the layer. Up, down, and right keep the layer armed so repeated presses keep working. A second Message press or a two-second timeout disarms it. |
| `tca8418` | TCA8418 4x10 matrix keyboard | `i2c=<bus> addr=0x34`; optional `irq=<pin> backlight=<pwm-pin>` | Polls the key-event FIFO and publishes keyboard input; the optional PWM binding controls keyboard backlight brightness. |
| `tab5-keyboard` | M5Stack Tab5 70-key keyboard | `i2c=<bus> addr=0x6d [irq=<pin>]` | Normal-mode press/release events, runtime keymaps, optional active-low interrupt with polling fallback. Accepts an already configured address from `0x08` to `0x77`. |
| `tca8418` | Generic TCA8418 matrix keyboard | `i2c=<bus> addr=0x34 [rows=1..8] [cols=1..10] [reset=<pin>] [irq=<pin>]` | Defaults to an 8x10 reference map; supports user mappings through `input keymap`. |
| `lilygo-pager-keyboard` | LilyGO T-LoRa-Pager keyboard | `i2c=<bus> addr=0x34 [reset=<pin>] [irq=<pin>] [backlight=<pwm-pin>]` | Fixed 4x10 Pager wiring and symbol layer, with optional PWM backlight. |
| `cl32-core` | Integrated CL-32 ATmega808 controller | `i2c=<bus> addr=0x08` | Fixed CL-32-only `core0`; polls keyboard press/release events into `keyboard0` and provides `battery0` from the AVR voltage, USB-power, and charging state. It is not runtime-probeable or detachable. |
| `gpio-keys` | Active-low pull-up buttons | One or more `key:<name>=<gpio>` bindings | Publishes press/release keyboard events and releases all GPIO claims on detach. |
| `ps2-keyboard` | PS/2 scan-code set 2 keyboard | `ps2=<bus>` | Publishes canonical keyboard press/release events from an exclusive PS/2 bus. |
| `ps2-mouse` | Standard three-button PS/2 mouse | `ps2=<bus>` | Enables reporting and publishes relative pointer motion and button events. |
| `analog-joystick` | Two-axis analog joystick | `x=<scalar-stream> y=<scalar-stream> min=<value> center=<value> max=<value>`; optional `deadzone=<value>` | Normalizes two scalar streams into X/Y axis events without generating keys. |
| `rotary-encoder` | Quadrature rotary encoder | `a=<pin> b=<pin>` | Publishes Up or Down input for each decoded detent. |
| `ft6336` | Board-integrated FT6336 touch controller | Board-defined I2C, address, reset, and IRQ bindings | Publishes absolute pointer events as a default board attachment. |
| `gt911` | GT911 capacitive touch controller | `i2c=<bus> addr=0x5d irq=<pin> rotation=<0..3>`; optional `alt_addr=0x14 reset=<pin> power=<pin> active=<0|1> home_key=<numeric-key>` | Polls absolute pointer events into the shared input service and retains state between fresh frames. Optional reset performs interrupt-pin address selection; optional power controls the touch rail (default active high). Both declared addresses are claimed. The Home pad can emit a key tap on release. |
| `mgc3130` | MGC3130/Skywriter 3D gesture sensor | `i2c=<bus> addr=0x42|0x43 transfer=<pin> reset=<pin> rotation=<0..3>`; optional `airwheel=0|1` | Registers in the `gesture` input class and uses the open-drain transfer handshake to publish normalized X/Y/Z axes, recognized gesture/tap/AirWheel events, and buttonless display-scaled absolute hover motion. It is not registered as a touch source, because the five electrode regions do not provide a continuous drawing surface. AirWheel defaults on; set it to 0 to make the mutually exclusive circle gestures available. Use `gesture bind` to map its generic gesture events to local keys, scripts, or other shell commands. |
| `pcf85063` | PCF85063 real-time clock | `i2c=<bus> addr=0x51`; optional `irq=<pin>` | Registers an RTC provider with wall-clock, alarm, timer, and optional interrupt support. |
| `pcf8563` | PCF8563 / BM8563 real-time clock | `i2c=<bus> addr=0x51`; optional `irq=<pin>` | Registers an RTC provider with calendar, minute-resolution alarms, countdown, and optional active-low interrupt support. Calendar dates use 2000–2099 with the century bit cleared. Low-voltage status marks the clock unreliable until the date is set. Nonzero alarm seconds are unsupported; countdown accepts 1–255 seconds or whole minutes up to 255 minutes. |
| `shtc3` | SHTC3 temperature and humidity sensor | `i2c=<bus> addr=0x70` | Registers temperature and humidity providers. |
| `battery-adc` | ADC battery monitor | `adc=<pin> divider=<1000..10000>` | Registers a battery provider using the configured divider ratio in thousandths. |
| `bq27220` | BQ27220 fuel gauge | `i2c=<bus> addr=0x55` | Registers a battery provider backed by the fuel gauge. |
| `max17048` | MAX17048 fuel gauge | `i2c=<bus> addr=0x36` | Registers a battery provider with gauge-reported voltage and state of charge. The chip does not report charger or external-power state, so charging remains unknown unless another provider supplies it. |
| `cw2017` | CW2017 fuel gauge | `i2c=<bus> addr=0x63` | Registers a battery provider with cell voltage and state of charge. Preserves the resident cell-specific BATINFO profile. Without a loaded profile or valid SOC, battery percentage uses the service's voltage estimate. Charging and external-power state are not reported by the gauge. |
| `bq25896` | BQ25896 battery charger | `i2c=<bus> addr=0x6b`; optional `charge_current=<0..3008>` and `charge_voltage=<3840..4608>` | Registers a charger provider; optional values configure milliamps and millivolts within the device's supported steps. |
| `xl9555` | XL9555 16-line GPIO controller | `i2c=<bus> addr=<0x20..0x27>`; optional `output=<0..65535> direction=<0..65535>` | Registers named digital control lines such as `gpiox0:4`; initial output latches are applied before output directions. |
| `ublox-mia-m10q` | u-blox MIA-M10Q GNSS receiver | `uart=<bus>`; optional `power=<gpio-or-controller-line>` | Registers a GNSS provider and controls its optional power line around the UART-backed receiver. |
| `nmea` | NMEA 0183 GNSS receiver | `uart=<bus>`; optional `power=<gpio-or-controller-line> active=0|1` (both or neither) and `alt_baud=<4800..921600>` | Registers a GNSS provider that parses streamed RMC and GGA sentences. While the receiver is powered, a background reader drains the stream and `gnss fix` answers from the latest sentences (at most 2.5 s old), so fix requests never wait on the receiver's epoch period. Power-on detection listens at the UART's configured rate, then at `alt_baud` if one is given, so receivers with differing factory bauds work unmodified; when neither is heard the configured rate is restored. A switched receiver's enable polarity is stated with `active`, never guessed. RMC/GGA do not provide position accuracy or PDOP; these fields remain zero (unavailable). Failed power-off retains the receiver registration so the operation can be retried. |
| `sim7670` | SIM7670 LTE modem and GNSS receiver | `uart=<bus>`; optional active-high `power=<gpio-or-controller-line>` and active-low `reset=<gpio-or-controller-line>` | Registers modem profile, power/reset, AT/status, GNSS, and a generic IPv4 PPP service bound to the exclusively claimed UART. Without a reset line, the driver can reset through the power line. PPP-enabled targets reserve the UART for network frames until disconnected. |
| `st25r3916` | ST25R3916 NFC-A reader | `spi=<bus> cs=<pin> irq=<pin>`; optional `power=<gpio-or-controller-line>` | Registers an NFC provider for ISO14443A discovery and card reads. |
| `bhi260ap` | Bosch BHI260AP six-axis IMU | `i2c=<bus> addr=<address> irq=<pin>` | Uploads the sensor firmware during attachment and registers acceleration, angular-rate, and orientation measurements. |
| `qmi8658` | QMI8658 six-axis IMU | `i2c=<bus> addr=0x6a|0x6b` | Polls data-ready status and registers SI-unit acceleration and angular-rate measurements. The Waveshare board uses fixed `imu0` at `0x6a`. |
| `drv2605` | TI DRV2605 haptic controller | `i2c=<bus> addr=0x5a`; optional `power=<gpio-or-controller-line>` | Registers a haptic provider for the controller's ROM effects. |
| `sdmmc` | Native SD/MMC card slot | `clk=<pin> cmd=<pin> d0=<pin>`; optional four-bit set `d1=<pin> d2=<pin> d3=<pin>` and `power=<pin> active=<0|1>` | Built-in slots register as fixed `storage0`; runtime attachments mount removable FAT storage at `/sdcard`. Classic ESP32 uses its native slot-1 pins. An optional power rail cycles off for 80 ms, then on for 120 ms before card initialization (default active high). |
| `sdspi` | SPI microSD card adapter | `spi=<bus> cs=<pin>` | On boards without built-in SD, mounts removable FAT storage at `/sdcard`; run `disk umount` before detach. |
| `neopixel` | WS2812/NeoPixel GRB strip | `data=<pin> count=<1..256>` | Claims the data GPIO and registers a named strip for the `neopixel` command and script API. |
| `audio-pwm` | LEDC PWM mono audio output | `pwm=<pin>` | Claims the PWM GPIO and registers a 16 kHz mono playback device. One instance can be attached. |
| `esp32-camera` | DVP JPEG camera | `d0=<pin>` through `d7=<pin>`, `vsync=<pin> href=<pin> pclk=<pin> xclk=<pin>`; either `i2c=<bus>` or `siod=<pin> sioc=<pin>`; optional `pwdn=<pin> reset=<pin>` | ESP32-S3 with PSRAM; claims GPIOs and the single DVP capture peripheral. Named SCCB buses can be shared with other devices; direct-pin SCCB reserves the configured I2C controller. Registers the device name as an exclusive typed video source. Capture starts only on use. Busy owners or frames prevent detach. |
| `pcm1808` | PCM1808 four-wire I2S ADC | `mclk=<pin> bck=<pin> ws=<pin> dout=<pin>` | Requires `expansion_i2s`, claims four GPIOs and a runtime I2S controller, then registers a 16 kHz stereo capture device and stream. One instance can be attached. |
| `es7210` | ES7210 I2S microphone array | `i2c=<bus> i2s=<port> mclk=<pin> bck=<pin> ws=<pin> din=<pin>` | Requires I2C and `expansion_i2s`, claims its bindings, then registers a 16 kHz stereo capture device and stream with microphone-gain control. One instance can be attached. |
| `pcm5102` | PCM5102A three-wire I2S DAC | `i2s=<port> bck=<pin> din=<pin> rck=<pin>` | Requires `expansion_i2s`, claims the selected runtime I2S controller and three GPIOs, then registers a 16 kHz stereo playback device and stream. One instance can be attached. |
| `i2s-output` | Generic I2S DAC or speaker amplifier | `i2s=<port> bck=<pin> din=<pin> rck=<pin>` | Claims the selected runtime I2S controller and registers the same 16 kHz stereo playback device and stream for an integrated or external Philips-I2S receiver without assuming a specific DAC. |
| `es8311-es7210` | ES8311 playback with ES7210 capture | `i2c=<bus> i2s=<port> mclk=<pin> bck=<pin> ws=<pin> din=<pin> dout=<pin> pa=<pin>` | ESP32-S3 primary audio backend with stereo capture and playback. Waveshare registers it as fixed `audio0`. |
| `es8311-duplex` | ES8311 duplex codec | `i2c=<bus> i2s=<port> mclk=<pin> bck=<pin> ws=<pin> din=<pin> dout=<pin> pa=<pin>` | ESP32-S3 primary audio backend with mono codec capture and playback. Freenove registers it as fixed `audio0`. |
| `esp32-dac` | Classic ESP32 internal DAC | `pos=gpio25|gpio26`; optional `neg=gpio25|gpio26 amp=<pin> active=0|1` | Registers a primary playback backend. ODROID-GO and TTGO VGA32 provide fixed `audio0` attachments. |

Manual profiles are useful when another app or workflow operates the hardware
but SolarOS still needs to prevent conflicting claims:

```text
expansion attach manual radio0 spi0 cs=gpio10 irq=gpio4 reset=gpio5
expansion attach manual sensor0 i2c0 addr=0x40
expansion detach radio0
```

For camera SCCB control, use either an existing named I2C bus or both SIOD/SIOC
GPIO bindings. Supplying both modes, only one SCCB pin, or neither mode is
rejected. With `i2c=i2c0`, the bus supplies SDA/SCL wiring and retains its other
devices when the camera stops. The camera keeps a bus lease until detach;
`d0`..`d7`, `vsync`, `href`, `pclk`, and `xclk` remain GPIO bindings.
On CL32, the expansion connector's camera control signals use the board-owned
`i2c0`, so select `i2c=i2c0` alongside the camera's DVP pin assignments. After
attachment, `camera status` initializes and probes the camera, and
`camera capture <path>` captures a JPEG. The direct-pin mode retains
`siod=<pin> sioc=<pin>` and reserves the SCCB controller selected by the firmware.

Binding names may be explicit (`spi=spi0`, `i2c=i2c0`) or, where unambiguous,
supplied as positional bus names. `ce=` aliases `cs=` and `rst=` aliases
`reset=` for common module labels.

The name `display0` is reserved for the board's primary display attachment.
Use another name such as `lcd0` for a runtime-attached display. The seven
integrated-display drivers attach before display-service initialization when a
board profile declares them as fixed defaults. When a controller driver is not
already attached, a later attachment registers an auxiliary target for
`display test`, graphical sessions, and applications. Each controller driver
supports one attached instance. A fixed board default cannot be detached.

Select the RFM69 driver from the module variant, not from the requested power.
The `rfm69h` driver uses PA1 through 13 dBm, PA1+PA2 through 17 dBm, and applies
the datasheet high-power OCP/TestPA settings only during 18-20 dBm transmit.
Those settings are restored before standby, receive, or sleep. Both module
families require 3.3 V power and a band-appropriate antenna.

### WS2812/NeoPixel strip

Use a runtime-safe expansion GPIO for DIN. The driver uses an ESP32 RMT transmit
channel and supports up to 256 GRB pixels per attached strip:

```text
5V supply + -> strip 5V       supply GND -> strip GND and SolarOS board GND
GPIO1 -> level shifter -> strip DIN

expansion attach neopixel pixels0 data=gpio1 count=8
neopixel set pixels0 0 32 0 0
neopixel fill pixels0 0 0 16
neopixel clear pixels0
expansion detach pixels0
```

Use an external supply sized for the strip; full-white WS2812 pixels can draw
roughly 60 mA each. Do not power a multi-pixel strip from a board GPIO. A 3.3 V
data signal may work with short wiring when the strip supply is low enough, but
a 3.3-to-5 V logic-level shifter is the reliable arrangement. Put the usual
bulk capacitor across the strip supply and a small series resistor near DIN.

### LEDC PWM audio output

Use a runtime-safe PWM pin. The driver updates an 8-bit, 78.125 kHz LEDC carrier
from a GPTimer-paced 16 kHz mono PCM stream. It registers the attached name as
an audio device and `<name>.playback` as its stream:

```text
expansion attach audio-pwm pwm0 pwm=gpio1
audio device pwm0
audio default pwm0
aplay /audio/example.mp3
```

`audio default` is runtime-only because attached expansion devices are also
runtime-only. Run `audio default auto` to return to the first compatible output.
Detaching the selected device also returns selection to `auto`.

Do not connect a speaker directly to the GPIO. The pin provides a 3.3 V PWM
signal centered near 50 percent duty during silence. Use a reconstruction
low-pass filter, a DC-blocking/coupling stage, and an amplifier suitable for the
speaker impedance. Keep the board and amplifier grounds common. Stop playback
before detaching; detach reports busy while the playback stream is open.

### PCM5102A I2S audio output

Wire the module's BCK, DIN, and RCK pins to three runtime-safe output GPIOs and
connect SCK to ground. The driver is an I2S master in Philips format with
32-bit slots, so its 16-bit stereo stream supplies the 64 BCK cycles per frame
needed by the PCM5102A PLL at 16 kHz. The attached name becomes an audio device
and `<name>.playback` becomes an exclusive 16 kHz, signed 16-bit stereo PCM
sink:

```text
PCM5102A VCC -> module-rated supply   PCM5102A GND -> SolarOS GND
PCM5102A SCK -> GND                   PCM5102A BCK -> GPIO1
PCM5102A DIN -> GPIO2                 PCM5102A RCK -> GPIO3

expansion attach pcm5102 dac0 i2s=i2s1 bck=gpio1 din=gpio2 rck=gpio3
audio device dac0
audio default dac0
aplay /audio/example.mp3
```

The example pins are the Waveshare board's runtime-safe expansion GPIOs. Mono
streams are duplicated to left and right. Volume is applied in software before
samples reach I2S. Select a runtime-safe controller explicitly with the
`i2s=` binding; this leaves other I2S controllers available to onboard audio
or composite video. The
PCM5102A output is line level: use a powered input or a suitable amplifier, not
a passive speaker. Stop playback before detaching; detach reports busy while
the playback stream is open. Run `audio default auto` after testing to restore
automatic output selection.

### PCM1808 I2S audio input

The driver makes the ESP32 the I2S clock master. Configure the PCM1808 mode
pins before power-up: `MD1=LOW`, `MD0=LOW` selects slave mode and `FMT=LOW`
selects 24-bit Philips I2S. Wire the driver MCLK signal to the module's SCKI or
SCK pin. The PCM1808 mode pins are hardware straps, not runtime driver GPIOs:

```text
PCM1808 +5V -> SolarOS 5V               PCM1808 3.3V -> SolarOS 3.3V
PCM1808 GND -> SolarOS GND
PCM1808 MD1 -> GND                      PCM1808 MD0 -> GND
PCM1808 FMT -> GND                      PCM1808 SCKI/SCK -> GPIO1
PCM1808 BCK -> GPIO2                    PCM1808 LRCK -> GPIO3
PCM1808 DOUT -> GPIO17

expansion attach pcm1808 adc0 mclk=gpio1 bck=gpio2 ws=gpio3 dout=gpio17
audio device adc0
arecord -d 5 -i adc0.capture /sdcard/pcm1808.wav
```

The example uses all four runtime-safe Waveshare expansion GPIOs. The driver
generates 4.096 MHz MCLK and 64 BCK cycles per 16 kHz stereo frame. It receives
the converter's 24-bit I2S slots, keeps the most-significant 16 bits, and
publishes `adc0.capture` as an exclusive signed 16-bit stereo PCM source. It
uses the board's runtime I2S controller (I2S1 on current ESP32-S3 profiles), so
it can coexist with fixed onboard audio on I2S0 but not with another attachment
that owns I2S1. Stop recording before detach. A raw PCM1808 circuit requires
the analog supply, digital supply, reference components, and input network from
the manufacturer reference design; follow the breakout's own supply markings
when using a module. Common modules expose separate `+5V` analog and `3.3V`
digital supply pins; both rails must be powered.

## Wiring Examples

### PCD8544 on ESP32-S3-DevKitC-1

```text
VCC -> 3V3        GND -> GND
CLK/SCLK -> GPIO12
DIN/MOSI -> GPIO11
CE/CS -> GPIO10   DC -> GPIO4   RST -> GPIO5

expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5
display test lcd0
```

Wire a module backlight according to the module board and use suitable current
limiting when connecting it to 3V3.

### Waveshare 4.2-inch V2 e-paper on ESP32-S3-DevKitC-1

This driver is for the monochrome 400x300 V2 module with the UC8176-compatible
controller path, including driver-board revision 2.2. It is not the
red/black/white `(B)` module.

```text
VCC -> 3V3        GND -> GND
CLK -> GPIO12     DIN -> GPIO11
CS -> GPIO10      DC -> GPIO17
RST -> GPIO16     BUSY -> GPIO15

expansion attach ssd1683 epd0 spi=spi0 cs=gpio10 dc=gpio17 reset=gpio16 busy=gpio15
display test epd0
display mode epd0
display mode epd0 refresh=full
display mode epd0 refresh=fast
expansion detach epd0
```

BUSY is active high. `refresh=auto` is the default: it uses a full waveform for
the first changed frame, then refreshes only the framebuffer rectangle that
changed. After 19 partial updates it reinitializes the controller and performs
a full cleanup refresh. After each partial waveform, the driver synchronizes
the controller's current and previous RAM planes before accepting the next
frame. Unchanged frames are skipped. `refresh=fast` remains a
fast full-frame update. Detach sends the controller to deep sleep before it
releases the SPI and GPIO resources. E-paper is bistable, so the last image
remains visible.
Keep VCC and ESP32 logic at 3.3 V even though recent Waveshare driver boards can
also operate in a 5 V logic domain.
If `expansion detach epd0` reports that the device is busy, run `sessions` and
close the display session that owns `epd0` with `session close <id>` first.

### M5Stack Unit CardKB on ESP32-S3-DevKitC-1

CardKB uses a fixed I2C address of `0x5f`. Connect SDA and SCL to the pins of
the named I2C bus; the DevKit `i2c0` board definition supplies the exact pin
numbers shown by `expansion buses`.

```text
VCC -> 5V         GND -> GND
SDA -> I2C0 SDA   SCL -> I2C0 SCL

expansion attach cardkb cardkb0 i2c=i2c0 addr=0x5f
expansion devices
expansion detach cardkb0
```

Each I2C read returns one key value, or zero when no key is pending. Printable
characters, Enter, Escape, Tab, Backspace, Delete, and the four arrows feed the
shared SolarOS input path. CardKB reports one value after release, so host-side
key repeat is not available. Its values 128 through 175 are private Fn
combinations and are ignored instead of being confused with SolarOS logical
keys.

### M5Stack Tab5 Keyboard

The [Tab5 Keyboard (A164)](https://docs.m5stack.com/en/tab5/Tab5_Keyboard)
is a 3.3 V, 70-key I2C keyboard with a default address of `0x6d`. Connect
3.3 V, GND, SDA, and SCL to the corresponding board supply and named bus.
INT is optional; connect it to a free, input-capable GPIO and pass `irq=<pin>`.
Use `expansion status` and `expansion layout` to find the board's buses and pins.

The `waveshare_esp32_s3_sim7670g_4g_epaper` profile attaches this keyboard
automatically as fixed `keyboard0` on `i2c0` at `0x6d`, with INT on GPIO41.
GPIO40 is available for expansion.
Use `input test keyboard0` to inspect its event count and last event. Fixed
board attachments cannot be detached or attached a second time from the shell.

On boards without a fixed Tab5 attachment, attach a connected keyboard with:

```text
expansion attach tab5-keyboard keyboard0 i2c=i2c0 addr=0x6d
input sources
input keymap keyboard0 show
expansion detach keyboard0
```

For interrupt operation, append an available GPIO binding, for example
`irq=gpio1` when GPIO1 is free on the board. Falling INT wakes the keyboard
worker. It also polls every 50 ms to cover missed edges; without INT it polls
every 10 ms. These are worker intervals, not guaranteed input latency.

The driver selects Normal mode and maps both press and release events into
the common input service, including host key repeat. Aa acts as held Shift,
Ctrl and Alt are held modifiers, and Sym selects the printed symbol layer
while held. Character-mode latch and double-click behavior is not used.
Letters and navigation keys carry HID usages. Printed punctuation uses logical
character mappings without HID usages; raw HID forwarding requires a mapping
that supplies the appropriate usages and modifiers.

Physical IDs are `1 + row * 14 + column`, with rows `0..4` and columns `0..13`.
Use `input keymap keyboard0 load <path>` and `input keymap keyboard0 reset`
to customize or restore the built-in map. This driver uses runtime keymaps;
it is not a `SOLAR_OS_KEYMAP_PROFILE` desktop/build profile.

The firmware has a 32-event FIFO and no overflow status flag. A detected full
queue, malformed event, input queue failure, or I2C error releases tracked
keys and triggers reconfiguration with a cleared FIFO. Release and press keys
again after recovery. A controller reset or an undetected dropped event cannot
be reconstructed from the event-only protocol.

Detach stops the worker and removes its GPIO handler, releases keys, and
restores the previous keyboard mode and interrupt configuration. It leaves
the named I2C bus available to other devices. If restoring configuration fails,
ownership is retained so detach can be retried. Attaching clears old queued
events; RGB settings and the persistent I2C address are not changed.

Custom flavors can enable `tab5_keyboard = true` in `[groups]` or select
`expansion_tab5_keyboard` in `[packages]`. The full flavor includes it on
ESP32 and ESP32-S3 boards with expansion I2C support. `addr` selects the current
device address; it does not reprogram the keyboard.

### TCA8418 matrix keyboards and mappings

`tca8418`, `inputronic-keyboard`, and `lilygo-pager-keyboard` share the
same controller backend, FIFO recovery, and matrix-to-input mapper. Each
attachment owns one controller and input source. The fixed controller address
is `0x34`; multiple controllers require separate named I2C buses.
RESET and INT bindings are optional. RESET is active low and is pulsed before
the first probe. FIFO polling works with or without INT.

The generic `tca8418` driver accepts `rows=1..8` and `cols=1..10`, defaulting
to 8 rows and 10 columns. The branded drivers select their own fixed geometry
and mapping. Generic TCA8418 has no PWM dependency; the Pager profile owns
its optional backlight.

```text
expansion attach tca8418 keyboard1 i2c=i2c0 addr=0x34 rows=4 cols=10
input keymap keyboard1
input keymap keyboard1 load /sd/keymap.json
input keymap keyboard1 reset
```

The generic default is a reference wiring map. These are the keys under the
US keyboard layout; HID usages pass through the active SolarOS keyboard
layout for translation.

| Row | Columns 0 through 9 |
| --- | --- |
| 0 | Q W E R T Y U I O P |
| 1 | A S D F G H J K L Enter |
| 2 | Left Shift Z X C V B N M Backspace Left Control |
| 3 | Space Tab Escape Caps Lock Left Alt Left Down Up Right Delete |
| 4 | 1 2 3 4 5 6 7 8 9 0 |
| 5 | F1 F2 F3 F4 F5 F6 F7 F8 F9 F10 |
| 6 | Minus Equals Left bracket Right bracket Backslash Semicolon Quote Grave Comma Period |
| 7 | Left GUI Right Control Right Shift Right Alt Right GUI F11 F12 Insert Home End |

Rows and columns are zero-based in mapping files. The physical event ID is
`1 + row * 10 + col`, even when fewer than ten columns are enabled.
The controller's matrix geometry is selected at attachment; mapping files
cannot change it.

The input service manages each source's mapping, modifier state, symbol layer,
and tap/hold selector. `input keymap` lists mapping support; `input keymap
keyboard1 show` inspects the active map. See [source keymaps](input.keymap.md)
for physical-key selectors, compatible matrix selectors, JSON fields, and
Python/Lua capability queries. The existing row/column mapping files remain
valid for these three profiles.

The Pager's built-in profile uses Space as a tap/hold symbol selector and its
Caps-labelled key as a momentary Shift modifier. Unlabelled cells produce no
logical input. The Pager board manifest initializes `lilygo-pager-keyboard`;
scripts that previously selected `tca8418` for the Pager should use this name
to retain its wiring, symbol layer, and backlight binding. The generic
`tca8418` name now selects the reference map. Custom flavors can select the
`lilygo_pager_keyboard` hardware group for the Pager profile.

### Soldered Inputronic KEYBOARD

The standalone Inputronic KEYBOARD (SKU 333360) uses a TCA8418 at the fixed
I2C address `0x34`. Connect its easyC/Qwiic connector to a 3.3 V supply,
ground, and the SDA/SCL pins of a named I2C bus. Use `expansion bus` to
identify the bus and its pins. A board-owned bus can be shared with other
devices at different addresses. Enable the `inputronic_keyboard` hardware
group in custom flavors; it is included in `full`.

```text
expansion attach inputronic-keyboard keyboard1 i2c=i2c0 addr=0x34
input keyboard
expansion detach keyboard1
```

If RESET is wired to a host GPIO, add `reset=<gpio>` to pulse the active-low
reset and hold it high before probing the keyboard. Otherwise RESET must
have a pull-up to the keyboard supply. If INT is wired, `irq=<gpio>` reserves
that input with a pull-up; FIFO polling still runs without a host interrupt.
The custom 4G/e-paper profile binds RESET to GPIO41 and INT to GPIO40.

Letters use the active SolarOS keyboard layout. Caps Lock starts off and
toggles on each Caps press; Shift reverses letter case. Number and punctuation
keys retain the Inputronic symbol map: Shift+0 through Shift+9 produce
`= ! " # $ % & / ( )`, and Shift+semicolon, Shift+comma, and Shift+period
produce colon, semicolon, and colon. Control/Alt chords and modified arrows
use the common keyboard translation. Enter, Escape, Tab, Backspace, Delete,
arrows, Space, and F1 through F10 feed shells and foreground apps. Held keys
use the shared input repeat settings.

The six FN keys publish physical press/release events with logical key and
HID usage zero for applications to interpret. Their physical IDs are
FN1=78, FN2=79, FN3=17, FN4=18, FN5=19, and FN6=20. They do not emit text.
FIFO overflow or I2C failure clears held keys and modifiers; after recovery,
release and press any still-held key again. Detaching closes the keyboard
source and releases the expansion's bus/address claims. Multiple keyboards
can use separate named buses, each at `0x34`.

### RFM95W on ESP32-S3-DevKitC-1

The RFM95W is a 3.3 V device. Connect an antenna suitable for the module band
before transmitting.

```text
VCC -> 3V3        GND -> GND
SCK -> GPIO12     MISO -> GPIO13
MOSI -> GPIO11    NSS/CS -> GPIO4
RESET -> GPIO5

expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5
radio status radio0
```

The module and driver support FSK, GFSK, MSK, GMSK, OOK, and LoRa. The default
LoRa profile is 868 MHz, 125 kHz bandwidth, SF7, coding rate 4/5,
CRC enabled, explicit headers, sync word `0x12`, and 13 dBm transmit power.
The matching built-in profile applies those settings as one operation:

```text
radio profile apply radio0 lora-eu868
radio send radio0 "hello from SolarOS"
radio recv radio0 5000
```

The built-in `meshcore-eu868` profile is specifically for MeshCore companion
operation in the EU868 region: 869.618 MHz, 62.5 kHz, SF8, coding rate 4/8,
a 32-symbol preamble, private sync word `0x12`, CRC, variable length, and
14 dBm. MeshCore always requires an explicit profile:

```text
job start meshcore radio0 meshcore-eu868
```

`gfsk-eu868` and `ook-eu868` are also built in. Change both ends of a link to
the same profile before exchanging packets. A custom set of settings can be
captured in one of eight persistent NVS user profiles:

```text
radio profile apply radio0 gfsk-eu868
radio config radio0 bitrate 9600
radio profile save radio0 gfsk-9600
radio profile show gfsk-9600
```

Applying a profile leaves the radio in standby and rolls back the complete
configuration and prior state if the driver rejects it. User profiles preserve
every common radio setting, including addressing. Built-in profiles are read-only.
Selecting MSK or GMSK sets the deviation to one quarter of the bitrate, giving
the required modulation index of 0.5. GFSK and GMSK enable Gaussian shaping
with BT=1.0.

FSK-family and OOK packet payloads are limited to 64 bytes by the modem FIFO;
LoRa payloads may contain up to 255 bytes. Fixed length zero selects the
FSK/OOK unlimited FIFO-stream mode used by services such as POCSAG.

The driver polls the radio status registers, so DIO0/IRQ is optional. An IRQ
binding can still be reserved for future interrupt-driven operation.

### SSD1306 or SH1106 on SolarTerm

```text
VCC -> 3V3        GND -> GND
SDA -> GPIO13     SCL -> GPIO14

i2c scan i2c0
expansion attach ssd1306 oled0 i2c=i2c0 addr=0x3c
display test oled0
```

Common modules answer at `0x3c` or `0x3d`. If the image is shifted two pixels
left with two uninitialized columns on the right, reattach it as SH1106:

```text
expansion detach oled0
expansion attach sh1106 oled0 i2c=i2c0 addr=0x3c
display test oled0
```

After an auxiliary display is attached, it can also host a shell session:

```text
session create shell oled0
```

## Quick reference

Inspect runtime-safe pins and buses before attaching hardware. Use the `io`
application or the `gpio`, `adc`, `pwm`, `i2c`, `spi`, `uart`, `bus`, and
`expansion` commands as documented here. Resource ownership prevents two
drivers, jobs, or sessions from claiming the same hardware concurrently.
