+++
id = "usb.host"
title = "USB host input"
section = "hardware"
summary = "Use USB keyboards and mice through the ESP32-S3 internal PHY"
aliases = ["usb-host"]
keywords = "usb host hid keyboard mouse cdc otg phy power"
packages_any = ["service_usb_host"]
+++
# USB host input

SolarTerm and ESP32-S3 DevKitC-1 N16R8 support explicit runtime USB mode
switching when `service.usb-host` is compiled. Every boot starts in device
mode, with native USB CDC and Serial/JTAG available. The selected mode is
volatile and is not saved across a restart.

## Connect hardware

Host mode uses GPIO19 for D− and GPIO20 for D+. Connect these signals and
ground to a suitable USB host connector. SolarTerm exposes them on its
expansion header. On DevKitC, use the native USB signals; the separate
USB–UART connector remains a UART console.

Provide the peripheral with a suitable regulated 5 V supply and common
ground. These profiles have no software-controlled VBUS power switch and do
not supply battery-powered USB host power automatically. Arrange current
limiting and prevent backfeeding between supplies. A powered adapter or hub
does not by itself establish correct host wiring or guarantee hub support.
Disconnect a PC from the native USB data lines before connecting a peripheral.

The internal PHY belongs to one controller at a time. Native USB CDC/JTAG is
unavailable while hosting. Use the display shell, UART, or a network shell
to switch modes. An existing CDC shell, bridge, transfer, or logging job must
release `cdc0` first; the command reports its owner when it is busy.

The reTerminal Sticky is ineligible: its USB connector uses a USB–UART bridge
and GPIO19/20 carry its microphone signals. Classic ESP32 boards have no
native USB-OTG host controller. Other S3 boards require a reviewed board
profile and suitable wiring before enabling the `USB_HOST` capability.

## Switch modes

```text
usb status
usb mode host
usb devices
input sources
input test usb-kbd0
usb mode device
```

`usb devices` lists opened keyboard/mouse interfaces, including their address,
interface number, readiness, report count, and errors. Input source names are
`usb-kbdN` or `usb-mouseN`, with `N` identifying the interface slot. A composite
keyboard/mouse may occupy multiple slots. Unsupported interfaces are left idle.

Keyboards supply physical HID usages, modifiers, press/release events, and
normal SolarOS key repeat. Use `setterm keyboard us` or `de` for text layout.
Mice supply relative movement and the primary, secondary, and middle buttons.
Applications and Python/Lua scripts use the existing input APIs and focus
routing. Unplugging a peripheral releases its held input and removes its source.

The initial implementation selects HID boot protocol. General report-protocol
devices, NKRO layouts, media keys, gamepads, mouse wheels, and external hubs
are outside this interface. Boot-capable keyboards can operate in their boot
mode even if they also offer other report layouts.

## Power and recovery

Host mode holds the APB clock and prevents automatic CPU light sleep. Display
suspend keeps USB active, so keyboard input can resume the display. Explicit
sleep stops hosting and releases its input before sleeping, then re-enumerates
devices on wake or cancelled sleep. USB is not an explicit-sleep wake source.
The external peripheral supply remains under hardware control.

Switching to device mode disables the USB root port, drains HID disconnects,
and uninstalls the host stack before restoring CDC. A failed host startup
attempts to restore device mode. A failed cleanup retains USB ownership and
reports `fault`; retry `usb mode device`. Restarting also returns to device
mode. USB tasks, queues, peripheral buffers, and power locks are created on
demand and released when hosting stops.

## Quick reference

`usb status` shows the current mode and errors. `usb devices` lists boot
keyboard/mouse interfaces. Arrange external 5 V peripheral power and host
wiring on GPIO19/20, then run `usb mode host` from a display, UART, or network
shell. Release any `cdc0` owner first. Use `usb mode device` to restore native
CDC/JTAG or retry recovery. Keyboard and mouse sources appear as `usb-kbdN`
and `usb-mouseN` in `input sources`. Every reboot starts in device mode.
