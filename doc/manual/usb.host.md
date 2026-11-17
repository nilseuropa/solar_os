+++
id = "usb.host"
title = "USB host"
section = "hardware"
summary = "Select USB off/device/host mode and use input, serial adapters, or FAT storage"
aliases = ["usb-host"]
keywords = "usb off device host gpio pins release hub hid keyboard mouse joystick gamepad controller cdc otg phy power storage msc flash drive floppy ufi cbi fat usb0"
packages_any = ["service_usb_host", "service_usb_storage", "service_usb_serial"]
+++
# USB host

ESP32-S3 targets with accessible native USB D−/D+ signals support explicit
runtime USB mode switching when `service.usb-host` is compiled. Access may be
through a native USB connector, expansion header, or breakout pads; a
USB–UART connector alone does not provide native USB data access.

Boards with native CDC normally boot in device mode. Watcher explicitly boots
with USB off; boards without native CDC also boot off. Mode selection is
volatile: restarting restores the board's boot policy. Native CDC support is
not required for hosting. Off-at-boot takes effect during SolarOS startup and
does not change the chip's ROM boot behavior.

## Release the USB pins

`usb mode off` stops USB, disables its data pads and pull resistors, and
releases GPIO19/20. Both pins return to a disabled, high-impedance GPIO state.
Use them through the ordinary GPIO, ADC, PWM, and runtime bus APIs where the
board exposes them as free or releasable expansion pins. Pins declared fixed
for board wiring remain unavailable to general GPIO even with USB off.

`usb mode device` enables the compiled native CDC/Serial-JTAG device;
`usb mode host` enables peripherals. Either active mode claims the data pair
and the internal PHY. An existing GPIO or peripheral owner prevents USB from
starting; release that owner first. Active USB likewise prevents another
service from taking the pins. Switching off never stops another pin owner or
changes its configuration. On profiles without native CDC, the older
`usb mode device` command remains an alias for off.

Close USB files and release `cdc0` or USB serial owners before switching off.
Run mode changes from the display, UART, or a network shell when the USB
connection itself will disappear. Stopping a logging job alone does not
disable CDC; use `usb mode off` to release the USB pins.

Software pin ownership cannot disconnect board wiring or an attached USB
cable. Isolate the native USB data pair from active USB hardware before using
it for unrelated GPIO signals.

## USB hubs

External USB hubs allow supported keyboards, mice, gamepads, USB serial
adapters, and one USB storage drive to share the native USB port. Connect the hub's upstream port
to SolarOS and the peripherals to its downstream ports. Use an externally
powered hub when the board's host supply cannot power all attached devices.
`usb mode host` starts enumeration; `usb devices` lists the supported
peripheral interfaces after they appear, including devices behind a hub.

All hubs and peripherals share the ESP32-S3's eight USB host channels.
Each physical device needs a channel for control transfers, and each claimed
endpoint needs another. A typical hub consumes two channels. The number of
usable peripherals therefore depends on their interfaces, not just the hub's
port count. For example, a hub, separate keyboard, separate mouse, and BOT
storage drive need at least nine channels and cannot all be active together. A combined
keyboard/mouse receiver can use fewer channels than two separate USB devices.
SolarOS routes up to eight HID interfaces within the hardware channel limit.

Unplugging a peripheral removes its input sources or storage mount. Unplugging
the hub removes all of its attached peripherals. Close files and unmount
USB storage before disconnecting the drive or hub.

## USB storage

With `service.usb-storage`, one BOT/SCSI USB flash drive connected directly
or through a hub mounts at `/usb0` after `usb mode host`. UFI/CBI USB floppy drives
(subclass `04h`, protocol `00h`) are supported as well. FAT12, FAT16, and FAT32 are
supported; exFAT and NTFS are not. Use `usb devices` and `disk lsblk` to inspect
the drive. Close files, then run `disk umount usb0` before unplugging it.
Use `disk mount usb0` to remount a drive that is still connected. USB media is
never automatically formatted. Empty floppy drives report `no disk`, and a
write-protected disk is shown as such in `usb status`. See `help storage` for
disk insertion, removal, and sleep behavior.
If a drive stops responding, close its files and run `usb mode device` followed
by `usb mode host` to reconnect it.
Only the first detected drive is selected. Additional storage drives are
ignored; unplug and reconnect an ignored drive after removing the selected
drive to use it as `/usb0`.

## USB serial

With `service.usb-serial`, host mode opens CDC-ACM interfaces, Espressif native
USB Serial/JTAG (`303a:1001`), CP210x (`10c4:ea60`, `ea70`, `ea71`), and
CH340/CH341 (`1a86:7522`, `7523`, `5523`). Composite devices can expose serial
and HID interfaces together, such as an Arduino Leonardo. FTDI and CH9102
adapters are not included.

```text
usb mode host
usb serial
usb serial usb-serial0 baud=115200 bits=8 parity=none stop=1 dtr=1 rts=0
com usb-serial0
```

Ports are named `usb-serial0` through `usb-serial3` and appear in `port list`
and `stream list`. Available host channels can limit the number of connected
interfaces below four. Each port starts at 115200 baud, 8 data bits, no parity,
one stop bit, DTR asserted, and RTS deasserted. Native Serial/JTAG uses USB
speed rather than the baud setting and supports only 8N1.

`usb serial PORT` reads the cached settings. Configure a free port with
`baud=1..4000000`, `bits=5..8`, `parity=none|odd|even|mark|space`, `stop=1|1.5|2`,
`dtr=0|1`, or `rts=0|1`. The device driver can reject unsupported settings.
`usb serial PORT break=MS` sends a break request (0 through 65535); not all
adapters implement it. DTR/RTS can reset a connected board or select its
bootloader, depending on that board's wiring.

`com`, stream consumers, and `flash` take exclusive ownership. Close the owner
before changing settings, switching out of host mode, or entering sleep.
Removal makes existing I/O fail. A removed port's name stays reserved until its
owner releases it; an old handle cannot attach to a replacement device.
Reconnect a device if enumeration failed because no port or USB channel was
available. Each port has a 2048-byte receive queue; `usb serial` reports RX,
TX, and dropped-byte counts.

Python and Lua use `solaros.usb_serial.configure(name[, options])` and
`send_break(name, duration_ms)` for configuration, then the byte stream API
for data. See [Python peripherals](python.hardware.md),
[Lua peripherals](lua.hardware.md), and [Flash another ESP board](flash.md).

## Connect hardware

Host mode uses GPIO19 for D− and GPIO20 for D+. Connect these signals and
ground to a suitable USB host connector. The following target profiles enable
`USB_HOST`; derived profiles inherit the capability from their base board.

| Target | Native USB data access |
| --- | --- |
| `solar_term` | Native USB connector and GPIO expansion header |
| `seeed_sensecap_watcher` | J2 GPIO19/20; native USB defaults off, console on the display |
| `esp32_s3_devkitc1_n16r8`, `devkitc1_epaper_workbench` | Native USB connector and GPIO19/20 headers |
| `cl_32` | Native USB connector and EX1 D−/D+ pins |
| `goouuu_esp32_s3cam` | Native USB connector and J2 GPIO19/20 pins |
| `freenove_esp32_s3_display_4_0` | Native USB-C connector |
| `qdtech_es3n28p`, `qdtech_es3c28p` | Native USB-C connector |
| `t_deck_plus`, `t_lora_pager` | Native USB-C connector |
| `waveshare_esp32_s3_epaper_3_97`, `xteink_x4_pro` | Native USB-C connector |
| `m5stack_papermono` | Native USB-C connector; power sink only |
| `elecrow_crowpanel_esp32_s3_4_2_epaper`, `elecrow_crowpanel_esp32_s3_5_79_epaper` | U5 GPIO19/20; USB-C is a USB–UART console |
| `solar_writer` | U5 GPIO19/20 and the I/O board's USB host connector; GPIO18 switches VBUS automatically |
| `waveshare_esp32_s3_sim7670g_4g`, `waveshare_esp32_s3_sim7670g_4g_epaper` | P3 GPIO19/20, shared with the board's USB mux circuitry |

The reTerminal E1003 uses GPIO19/20 for onboard I2C. Its USB-C connector
exposes a USB–UART bridge, so native USB host mode is unavailable on this target.

USB–UART consoles remain available during hosting. On the SIM7670G board,
set the hardware switches so the native data pair is isolated from the
onboard hub, modem, and PC before connecting a peripheral directly. USB host
mode supports the HID and storage devices described here; it does not provide
USB modem support.

On CrowPanel, GPIO19/20 remain general expansion pins while USB is off.
Release any GPIO, ADC, PWM, or bus owner before enabling hosting. The host
service claims both pins and the USB PHY until hosting stops, including
while suspended for explicit sleep.

On the Freenove FNK0104S, these signals connect to the native USB-C port.
Its CC pins have fixed power-sink resistors, and its VBUS input feeds the
board supply through a diode. The board does not supply host VBUS or boost
battery voltage to 5 V. Use a suitable externally powered data breakout or
cable that connects the peripheral directly and provides its 5 V supply;
a passive USB-C host adapter alone is insufficient.

On the M5Stack Paper Mono, the native USB-C port carries GPIO19/20. Its CC
pins have 5.1 kΩ power-sink resistors and VBUS feeds only the AW32901
protection switch and IP2315 charger, so the board never supplies host VBUS.
Use a suitable externally powered data adapter, as on the Freenove board.

On SolarWriter, `usb mode host` raises GPIO18 to enable the onboard VBUS
switch after the host drivers start. VBUS is switched off when hosting stops,
during explicit sleep, or on startup rollback, and restored when hosting
resumes. GPIO21 keeps the shared boost/audio rail enabled independently.
`usb status` reports the commanded VBUS switch state, not a voltage measurement.
The TPS2553 provides hardware current limiting; GPIO38 fault monitoring is
not implemented.

Other supported profiles require a suitable regulated external 5 V peripheral
supply and common ground; the host service does not configure their power
switches or charger boost modes. Arrange current limiting and prevent
backfeeding between supplies. A powered adapter or hub still requires
correct host data wiring and isolation from any PC USB data connection.
Disconnect a PC from the native USB data lines before connecting a peripheral.

The internal PHY belongs to one controller at a time. Native USB CDC/JTAG is
unavailable while hosting. Use the display shell, UART, or a network shell
to switch modes. An existing CDC shell, bridge, transfer, or logging job must
release `cdc0` first; the command reports its owner when it is busy.

The reTerminal Sticky is ineligible on stock wiring: its USB connector uses
a USB–UART bridge and GPIO19/20 carry its microphone signals. ThinkNode M9
uses GPIO19 for LCD tearing effect and GPIO20 for keyboard I2C; it has no
usable external native USB pair. Classic ESP32 boards have no native USB-OTG
host controller. Custom S3 profiles enable `USB_HOST` when both native data
signals are accessible and available for USB use.

## Switch modes

```text
usb status
usb mode off
usb mode host
usb devices
input sources
input test usb-kbd0
usb mode device
usb mode off
```

`usb mode host` enables hosting and confirms the selected mode. Enumeration
continues asynchronously; use `usb status` or `usb devices` afterward to inspect
the interfaces that have appeared. Mode changes do not list devices.

`usb devices` lists supported HID interfaces, including their address,
interface number, readiness, report count, and errors. Input source names are
`usb-kbdN`, `usb-mouseN`, or `usb-joyN`, with `N` identifying the interface slot.
A composite device may occupy multiple slots. Unsupported interfaces are ignored.

`usb devices` also shows each interface's USB VID:PID, the protocol reported
by the device, and any protocol-check or idle-setup error. A rejected protocol
request prevents the interface from being marked ready after fallback. Devices that
reject the optional protocol check or idle setup can still receive input;
their protocol is shown as unverified when it could not be read back.

With USB storage included, `usb status` and `usb devices` also show the drive's
VID:PID, capacity, mount state, open-file count, transport, and write protection.
An empty floppy drive appears as `no disk`; use `disk lsblk` to inspect mounted
USB storage and `ls /usb0` to browse its files.

## Keyboards and mice

Keyboards supply physical HID usages, modifiers, press/release events, and
normal SolarOS key repeat. Use `setterm keyboard us` or `de` for text layout.
Mice supply relative movement and the primary, secondary, and middle buttons.
Applications and Python/Lua scripts use the existing input APIs and focus
routing. Unplugging a peripheral releases its held input and removes its source.

Boot-capable keyboards use boot protocol. Non-boot keyboards, including the
Arduino Leonardo keyboard, are identified from their HID Keyboard application
collection and use report protocol. Their input layout can contain up to six
8-bit key usages in one array, optional modifier bits, and a report-ID prefix.
Field offsets come from the descriptor. One keyboard input report is selected
per interface; other report IDs are ignored without releasing held keys.
Bitmap/NKRO keyboards and key state split across multiple reports are not
supported.

Mouse interfaces must advertise boot capability. SolarOS prefers report
protocol when the HID descriptor
contains a supported relative pointer layout, then falls back to boot protocol
if that layout is unavailable or the receiver rejects report mode. This also
supports receivers that accept boot mode but send mouse input only in report mode.

Mouse report layouts can use report IDs, packed buttons, and signed X/Y fields
up to 16 bits. SolarOS uses one relative pointer report per interface and ignores
other report IDs, including media and system controls. Only the primary,
secondary, and middle buttons are routed. Media keys,
mouse wheels and absolute pointers remain unsupported.

## Joysticks and gamepads

Standard USB HID Joystick and Game Pad application collections are identified
from their report descriptors. These interfaces do not need boot capability.
One controller application is supported per interface, with controls spread
across up to eight report IDs and input reports up to 64 bytes.

| Control | SolarOS input |
| --- | --- |
| X, Y, Z, Rx, Ry, Rz | Axis events normalized to −32767…32767; the declared midpoint is zero |
| Four-way or eight-way hat | Up/down/left/right key presses and releases; diagonals hold two keys |
| Separate HID D-pad directions | The same directional keys when no hat is present |
| Buttons 1…16 | Key events with `physical_key` 1…16 and `key=0`, `usage=0` |

Signed and unsigned absolute axis fields up to 32 bits and packed button fields
are supported. Other report IDs are ignored without releasing controls held by
the controller. Hats use physical IDs `0x100`…`0x103` for up, down, left, right.
Out-of-range hat values are neutral. Extra axes and buttons are ignored.

Axis-aware applications and Python/Lua scripts use the existing input events.
Buttons do not produce text characters; applications can map their physical
button numbers to actions. No automatic analog-stick-to-arrow mapping or
dead zone is applied. Unplugging releases held buttons/directions and queues
neutral axis values before removing the source.

```text
usb mode host
usb devices
input joystick
input test usb-joy0
```

This supports standard HID reports. Vendor-specific controller protocols such
as XInput, force feedback, and controller LEDs require separate drivers.

## Power and recovery

Host mode holds the APB clock and prevents automatic CPU light sleep. Display
suspend keeps USB active, so keyboard input can resume the display. Explicit
sleep stops hosting and releases its input before sleeping, then re-enumerates
devices on wake or cancelled sleep. Close files on `/usb0` and unmount disk
images backed by USB files first: open files prevent mode changes and explicit
sleep. USB is not an explicit-sleep wake source.
The external peripheral supply remains under hardware control.

Switching out of host mode disables the USB root port, drains HID disconnects,
and uninstalls the host stack. Device mode then restores CDC; off releases the
pins. A failed host startup attempts to restore the preceding off or device
mode. Failed cleanup retains USB ownership and reports `fault`; retry
`usb mode off` before enabling USB again. During explicit sleep, host mode
retains its pin claims until wake or a failed resume has safely stopped USB.

## Quick reference

Arrange external 5 V peripheral power and host wiring on GPIO19/20, then run
`usb mode host` from a display, UART, or network shell. Release any `cdc0` or
GPIO19/20 owner first. `usb status` shows the mode and errors; `usb devices`
lists supported input interfaces and, with `service.usb-storage`, one connected
FAT USB flash or UFI/CBI floppy drive. Input sources appear as `usb-kbdN`,
`usb-mouseN`, and `usb-joyN`; storage mounts at `/usb0` and appears in
`disk lsblk`. Close USB files and run `disk umount usb0` before unplugging.
Use `usb mode off` to stop USB and release GPIO19/20, or `usb mode device`
to restore compiled native CDC/JTAG. Release conflicting owners first.
Watcher boots with USB off and its GPIO19/20 free; other CDC profiles normally
boot in device mode. Every restart restores the board policy.
