# Expansion Ports

SolarOS treats an expansion port as a board-described collection of resources,
not as one fixed connector standard. A board may expose individual GPIO pins,
named I2C, SPI, or UART buses, or free pins that can be routed to an approved
spare peripheral host at runtime.

Use `expansion status` and `gpio list` on the running device for the authoritative
view. The available resources depend on the board and the compiled firmware
flavor.

## Resource Model

| Term | Meaning | Ownership and lifetime |
| --- | --- | --- |
| Connector pin | A signal physically present on an expansion header or breakout. Physical presence does not make a pin safe for runtime control. | Described by the board profile. |
| Runtime GPIO | A connector pin approved for direct `gpio` and 1-Wire use, and for `adc` or `pwm` where the board tables allow it. | Claimed while a service or attached device uses it. |
| Board-defined bus | A named bus with fixed pins, such as `i2c0` or `spi0`. | Registered at boot and cannot be removed. |
| Runtime bus | A named bus routed onto approved free pins and a spare hardware host. | Signal pins are claimed when the bus is created; the bus can be removed when idle. |
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
| Waveshare ESP32-S3-RLCD-4.2 | GPIO0-GPIO3, GPIO13, GPIO14, GPIO17-GPIO20, GPIO43, GPIO44 | GPIO1-GPIO3, GPIO17 | GPIO1-GPIO3, GPIO17 | GPIO0 is BOOT; GPIO13/GPIO14 are I2C; GPIO18 is KEY; GPIO19/GPIO20 are native USB; GPIO43/GPIO44 belong to `uart0` by default. |
| Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | GPIO3, GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO21, GPIO38 | GPIO8, GPIO9, GPIO14-GPIO20 | GPIO3 is physically exposed but blocked as a strapping pin. |
| ESP32-S3-DevKitC-1-N16R8 | ESP32-S3 signals broken out on the DevKitC headers | GPIO1, GPIO2, GPIO4-GPIO7, GPIO10, GPIO14-GPIO18, GPIO21, GPIO39-GPIO42, GPIO47 | GPIO1, GPIO2, GPIO4-GPIO7, GPIO10, GPIO14-GPIO18 | GPIO0/GPIO3/GPIO45/GPIO46 are strapping pins; GPIO19/GPIO20 are native USB; GPIO35-GPIO37 are Octal PSRAM; GPIO38/GPIO48 are reserved for either RGB LED revision; GPIO43/GPIO44 are `uart0`. |
| ODROID-GO | External IO GPIO4 and GPIO15 | GPIO4, GPIO15 | None | Both pins are also the allowed external chip-select slots on the shared VSPI bus. |

Power and ground pins are physical wiring resources and are not managed by the
SolarOS pin-claim system. Check the board schematic and the external module's
voltage and current requirements before connecting it.

### Named and Runtime Buses

| Board | Board-defined buses | Runtime-routable buses | Notes |
| --- | --- | --- | --- |
| Waveshare ESP32-S3-RLCD-4.2 | `i2c0`: SDA GPIO13, SCL GPIO14; `uart0`: TX GPIO43, RX GPIO44 | SPI on spare host `spi3`, using approved free pins | There is no fixed expansion SPI bus. The internal display SPI pins are not expansion pins. |
| Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | None | None | The SSD1683 and microSD buses are internal board resources, not expansion buses. External modules currently use direct GPIO, ADC, PWM, or 1-Wire. |
| ESP32-S3-DevKitC-1-N16R8 | `i2c0`: SDA GPIO8, SCL GPIO9; `spi0`: SCK GPIO12, MISO GPIO13, MOSI GPIO11, CS GPIO10/GPIO5/GPIO6/GPIO7 | SPI on spare host `spi3`, using approved free pins | The board-defined `spi0` is the normal expansion SPI bus. |
| ODROID-GO | `spi0`: SCK GPIO18, MISO GPIO19, MOSI GPIO23, CS GPIO15/GPIO4 | None | VSPI is shared with onboard TFT and SD devices; external devices use their own allowed CS slot. |

I2C and SPI buses accept shared logical leases. UART and registered 1-Wire bus
instances are exclusive. Registered 1-Wire buses appear in expansion status
and can be addressed by name. Bus names are unique across protocols.

Only SPI buses can currently be created at runtime. Runtime I2C, UART, and
1-Wire bus creation is not implemented. Board-defined 1-Wire buses use the
common registry; the direct numeric form of the `onewire` command remains
available for runtime-safe GPIOs without creating a named expansion bus.

## Typical Workflow

Start by inspecting the live resource map and compiled drivers:

```text
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

On a board with an approved spare SPI host, create a bus before attaching the
device. Creating the bus claims SCLK, MOSI, and optional MISO immediately. Each
`cs=` option declares an allowed chip-select slot, which is claimed only when a
device attaches:

```text
expansion bus create spi spi1 host=spi3 sclk=gpio1 mosi=gpio2 miso=gpio3 cs=gpio17
expansion attach rfm69 radio0 spi=spi1 cs=gpio17
expansion detach radio0
expansion bus remove spi1
```

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
display, storage, I2C, USB, or strapping pins. A bus cannot be removed while it
has device leases, and board-defined buses can never be removed.

## Drivers and Bindings

Run `expansion drivers` on the device to see the exact compiled set.

| Driver | Device | Required bindings | Result after attach |
| --- | --- | --- | --- |
| `manual` | Resource-only profile | Any valid bus, address, chip-select, GPIO, ADC, or PWM bindings | Claims resources without initializing hardware. |
| `rfm69` | HopeRF RFM69 packet radio | `spi=<bus> cs=<pin>`; optional `irq=<pin> reset=<pin>` | Registers a packet-radio target for the `radio` command. |
| `pcd8544` | 84x48 SPI LCD | `spi=<bus> cs=<pin> dc=<pin> reset=<pin>` | Registers an auxiliary display target. |
| `ssd1306` | 128x64 I2C OLED | `i2c=<bus> addr=<address>` | Registers an auxiliary display target. |
| `sh1106` | 128x64 I2C OLED with SH1106 addressing | `i2c=<bus> addr=<address>` | Registers an auxiliary display target with the two-column offset. |

Manual profiles are useful when another app or workflow operates the hardware
but SolarOS still needs to prevent conflicting claims:

```text
expansion attach manual radio0 spi0 cs=gpio10 irq=gpio4 reset=gpio5
expansion attach manual sensor0 i2c0 addr=0x40
expansion detach radio0
```

Binding names may be explicit (`spi=spi0`, `i2c=i2c0`) or, where unambiguous,
supplied as positional bus names. `ce=` aliases `cs=` and `rst=` aliases
`reset=` for common module labels.

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

### SSD1306 or SH1106 on Waveshare ESP32-S3-RLCD-4.2

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
