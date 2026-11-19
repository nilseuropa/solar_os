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

## Build and run

From the root of the SolarOS checkout, start the display target:

```sh
pio run -e qemu_esp32s3_rgb -t qemu
```

For the headless target:

```sh
pio run -e qemu_esp32s3 -t qemu
```

The `qemu` target builds the firmware, downloads the pinned Espressif Xtensa
QEMU release for your host when needed, prepares a 16 MB virtual flash image,
and launches the emulator. There is no separate QEMU installation or image
merge command. PlatformIO caches the emulator in its `tool-solaros-qemu`
package and reuses it on later runs. Prebuilt tools are available for Linux
and macOS on x86-64 and ARM64, and Windows on x86-64.

On the display target, the QEMU window shows the screen and the launching
terminal carries the separate UART0 console. Connect the display keyboard as
described below. The headless target uses only the launching terminal.

Press **Ctrl+A, then X** in the launching terminal to stop QEMU. **Ctrl+A, then C**
switches between the UART0 console and QEMU's monitor. Closing the
display window also stops QEMU. **Ctrl+Alt+G** releases the mouse from the window.

Console output is saved to `.pio/build/<env>/uart0.log`.

### Host dependencies

QEMU needs its host system libraries. On Ubuntu or Debian:

```sh
sudo apt-get install libgcrypt20 libglib2.0-0 libpixman-1-0 libsdl2-2.0-0 libslirp0
```

On macOS:

```sh
brew install libgcrypt glib pixman sdl2 libslirp
```

### Tool versions

| Part | Version |
| --- | --- |
| QEMU | Espressif `esp-develop-9.2.2-20261109` |
| ESP-IDF | 5.5.5 (PlatformIO `framework-espidf`) |
| esptool | 5.4.0 |

### Build without launching

Omit `-t qemu` to compile the firmware without starting the emulator:

```sh
pio run -e qemu_esp32s3_rgb
```

### Virtual flash

Each environment keeps its flash image at `.pio/build/<env>/flash_image.bin`.
Rerunning the same build preserves settings and files written by the guest.
When any flash input changes, the launcher creates a fresh image. Cleaning
the build directory also removes the virtual flash and its contents.

## Keyboard

QEMU's ESP32-S3 machine has no emulated keyboard, USB host, or I2C, so keys
typed into the QEMU window do not reach SolarOS. The `qemu_esp32s3_rgb` board
reads a VT100 terminal on UART1 (`keyboard0`, driver `uart-keyboard`) instead,
which the launcher opens on TCP port 5555 on the local machine.

On Windows, run in a second console window, from the root of your SolarOS
checkout:

```
python scripts/qemu_uart_keys.py 5555
```

Keys typed in that window go to the display shell; the mouse wheel sends
Page Up and Page Down, which scroll the shell. Ctrl+] is SolarOS's app-exit
key; Ctrl+C ends the script.

On Linux and macOS, use `socat` in a second terminal:

```
socat -,rawer,escape=0x0f tcp:127.0.0.1:5555
```

Ctrl+O ends `socat`. There is no mouse wheel this way. Use ASCII input for now:
the UART keyboard currently interprets some UTF-8 bytes as special keys.

## Network

QEMU's ESP32-S3 machine has no Wi-Fi model, but an OpenCores Ethernet card.
On `qemu_esp32s3_rgb` the `qemu-eth` driver (`eth0`) brings it up with DHCP from
QEMU's user network (`-nic user,model=open_eth`, no setup on the host). While it
has an address, SolarOS reports it as a connected Wi-Fi station with the SSID
`qemu-eth`, so `curl`, `ntp`, web, SSH, and Python networking work unchanged.
Tested: DHCP, DNS, HTTP, NTP, and ICMP ping to internet hosts on a Windows host.

Run `ntp` once after boot to set the clock.

### Debugging the network

To record packets when launching QEMU manually, replace
`-nic user,model=open_eth` with these options:

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
- Changes to the firmware's flash inputs recreate the virtual flash image,
  clearing guest files and settings.

## Quick reference

Run `pio run -e qemu_esp32s3_rgb -t qemu` for the display target, or
`pio run -e qemu_esp32s3 -t qemu` for headless. PlatformIO builds the firmware,
installs the emulator, and prepares the flash image. Send display keys with
`scripts/qemu_uart_keys.py 5555` (Windows) or `socat` (Linux/macOS).
