+++
id = "qemu"
title = "Running SolarOS in QEMU"
section = "build"
summary = "Build and launch the qemu_esp32s3 boards in Espressif's QEMU, with display, keyboard, and network"
aliases = ["emulator", "qemu_esp32s3"]
keywords = "qemu emulator virtual esp32s3 qemu_esp32s3 qemu_esp32s3_rgb open_eth esp_rgb sdl uart keyboard development testing"
packages_any = []
+++
# Running SolarOS in QEMU

Two board targets run SolarOS in Espressif's QEMU, without hardware:

| Target | PlatformIO env | What you get |
| --- | --- | --- |
| `qemu_esp32s3` | `qemu_esp32s3` | Headless: the shell on UART0 (your terminal). |
| `qemu_esp32s3_rgb` | `qemu_esp32s3_rgb` | An 800x600 display in the QEMU window, a keyboard over UART1, a network, and the UART0 shell next to the display shell. |

## Tested versions

| Part | Version |
| --- | --- |
| QEMU | Espressif `esp-develop-9.2.2-20261109` |
| ESP-IDF | 5.5.5 (PlatformIO `framework-espidf`) |
| esptool | 5.4.0 |
| Host | Windows 11 |

Use QEMU 20261109 or newer.

## Get QEMU

Download the `qemu-xtensa-softmmu-esp_develop_9.2.2_20261109-<host>.tar.xz`
archive for your host from
<https://github.com/espressif/qemu/releases/tag/esp-develop-9.2.2-20261109>
and unpack it. The program is `qemu/bin/qemu-system-xtensa`.

## Build the image

From the root of your SolarOS checkout:

```
pio run -e qemu_esp32s3_rgb
cd .pio/build/qemu_esp32s3_rgb
esptool --chip esp32s3 merge-bin --pad-to-size 16MB -o flash_image.bin 0x0 firmware.factory.bin
```

Use `qemu_esp32s3` in both places for the headless board. PlatformIO installs
esptool in its own Python environment (`penv`); call it from there.

## Launch

Start QEMU from the build folder above (`.pio/build/<env>`), where
`flash_image.bin` lies. Every run takes these options:

```
qemu-system-xtensa -machine esp32s3 -m 8M -global driver=ssi_psram,property=is_octal,value=true -drive file=flash_image.bin,if=mtd,format=raw
```

Headless (`qemu_esp32s3`), with the shell in your terminal and a copy in `uart0.log`:

```
... -nographic -chardev stdio,id=con,logfile=uart0.log,signal=off -serial chardev:con
```

Display, keyboard, and network (`qemu_esp32s3_rgb`):

```
... -display sdl -chardev stdio,id=con,logfile=uart0.log,signal=off -serial chardev:con -serial tcp:127.0.0.1:5555,server=on,wait=off -nic user,model=open_eth
```

The QEMU window shows the display. The terminal you started QEMU from carries
the UART0 shell. Ctrl+Alt+G releases the mouse from the QEMU window.

## Keyboard

QEMU's ESP32-S3 machine has no emulated keyboard, USB host, or I2C, so keys
typed into the QEMU window do not reach SolarOS. The `qemu_esp32s3_rgb` board
reads a VT100 terminal on UART1 (`keyboard0`, driver `uart-keyboard`) instead,
which the launch line above opens on TCP port 5555.

On Windows, run in a second console window, from the root of your SolarOS
checkout:

```
python scripts/qemu_uart_keys.py 5555
```

Keys typed in that window go to the display shell; the mouse wheel sends
Page Up and Page Down, which scroll the shell. Ctrl+] is SolarOS's app-exit
key; Ctrl+C ends the script.

On Linux and macOS, `socat` can do the same (not run by the author):

```
socat -,rawer,escape=0x0f tcp:127.0.0.1:5555
```

Ctrl+O ends `socat`. There is no mouse wheel this way.

## Network

QEMU's ESP32-S3 machine has no Wi-Fi model, but an OpenCores Ethernet card.
On `qemu_esp32s3_rgb` the `qemu-eth` driver (`eth0`) brings it up with DHCP from
QEMU's user network (`-nic user,model=open_eth`, no setup on the host). While it
has an address, SolarOS reports it as a connected Wi-Fi station with the SSID
`qemu-eth`, so `curl`, `ntp`, web, SSH, and Python networking work unchanged.
Tested: DHCP, DNS, HTTP, NTP, and ICMP ping to internet hosts on a Windows host.

Run `ntp` once after boot to set the clock.

### Debugging the network

Not needed for normal use. To record every packet between SolarOS and QEMU's
network into `net.pcap`, start QEMU with these options in place of
`-nic user,model=open_eth`:

```
-netdev user,id=n0 -net nic,model=open_eth,netdev=n0 -object filter-dump,id=f0,netdev=n0,file=net.pcap
```

Each start writes a new `net.pcap`. To print its DHCP, ARP, DNS, and NTP
packets, one line each, run from the root of your SolarOS checkout:

```
python scripts/qemu_pcap_dns.py .pio/build/qemu_esp32s3_rgb/net.pcap
```

## Limitations

- No touch, mouse, or pointer input.
- No audio, battery, Bluetooth, or SD card.
- The board declares the DevKitC-1 capabilities `gpio`, `i2c`, `spi`, `pwm`,
  `usb_host`, `key`, and their `expansion_*` forms, so the same services build
  and boot as on hardware. QEMU's ESP32-S3 machine has no device behind them:
  GPIO inputs read 0 and writes do nothing, and I2C, SPI, PWM (LEDC), I2S, and
  USB host have no peripheral to talk to. Sources: Espressif's
  [QEMU feature table](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md)
  and `hw/xtensa/esp32s3.c` in the QEMU release above.
- Mono display only, 800x600 at most (the largest panel QEMU's `esp_rgb` model takes).
- Keys only through UART1, as above.
- Wi-Fi commands (`wifi scan`, `wifi connect`) have no radio to work with; the
  network comes only from `eth0`.
- No network on `qemu_esp32s3` (headless): it has no `eth0` device.
- `shutdown` on `qemu_esp32s3` prints its message but leaves the shell running.
  Likely cause, read from the code: shutdown waits for the BOOT key (GPIO0) to
  be released, and GPIO0 reads 0, the key's active level.
- Files written to flash last only until the next image is built: `merge-bin`
  writes a fresh flash image.

## Quick reference

Build `qemu_esp32s3_rgb` (or headless `qemu_esp32s3`) with PlatformIO, merge
`firmware.factory.bin` into a 16 MB `flash_image.bin` with esptool, and start
Espressif QEMU 20261109 or newer with `-machine esp32s3 -m 8M`, octal PSRAM, the
image as `mtd` drive, `-display sdl`, UART1 on TCP port 5555, and
`-nic user,model=open_eth`. Send keys with `scripts/qemu_uart_keys.py 5555`
(Windows) or `socat`. Mono 800x600, no touch, audio, battery, Bluetooth, or SD;
GPIO, I2C, SPI, PWM, and USB host are declared but have no device in QEMU.
