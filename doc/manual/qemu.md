+++
id = "qemu"
title = "Running SolarOS in QEMU"
section = "build"
summary = "Build and launch the qemu_esp32s3 boards in Espressif's QEMU, with a display, native input, and networking"
aliases = ["emulator", "qemu_esp32s3"]
keywords = "qemu emulator virtual esp32s3 qemu_esp32s3 qemu_esp32s3_rgb open_eth esp_rgb sdl uart keyboard development testing"
packages_any = []
+++
# Running SolarOS in QEMU

Two board targets run SolarOS in Espressif's QEMU, without hardware:

| Target | PlatformIO env | What you get |
| --- | --- | --- |
| `qemu_esp32s3` | `qemu_esp32s3` | Headless: the shell on UART0, exposed as a host serial device. |
| `qemu_esp32s3_rgb` | `qemu_esp32s3_rgb` | An 800x600 display in the QEMU window, native keyboard, touch or relative mouse input, networking, and a separate UART0 console. |

## Build and run

From the root of the SolarOS checkout, start the display target:

```sh
pio run -e qemu_esp32s3_rgb -t qemu
```

For the headless target:

```sh
pio run -e qemu_esp32s3 -t qemu
```

The `qemu` target builds the firmware, prepares a 16 MB virtual flash image,
and launches the emulator. Native input and host-directory storage use a cached
QEMU extension built from pinned Espressif source on Linux and macOS.
PlatformIO keeps the emulator in its `tool-solaros-qemu` package and reuses it
on later runs. A headless target with host storage disabled can use the prebuilt
Espressif tool instead. Prebuilt tools are available for Linux and macOS on
x86-64 and ARM64, and Windows on x86-64. Use WSL on Windows for the extended
emulator, or configure an executable built with the SolarOS extensions.

On Linux and macOS, UART0 is a host pseudo-terminal (PTY) on both targets.
The launcher prints its stable path, `.pio/build/<env>/uart0`, which points
to the allocated host device, such as `/dev/pts/7`. Open it in a serial terminal
or another program that uses serial ports. The launching terminal controls
QEMU's monitor independently of UART0. Type `quit` there to stop QEMU, or
`system_reset` to reboot the guest. The device link is removed when QEMU exits.

On the display target, type into the QEMU window to control the display shell.
Closing the display window also stops QEMU. **Ctrl+Alt+G** releases the mouse
from the window.

While a serial client is connected, UART0 output is saved to
`.pio/build/<env>/uart0.log`.

### Connect to UART0

From a separate terminal with VT100 support, disable host software flow control
so Ctrl+S/Ctrl+Q reach SolarOS, then use PlatformIO's raw monitor:

```sh
stty -ixon -ixoff
pio device monitor -p .pio/build/qemu_esp32s3_rgb/uart0 -b 115200 --raw --eol CR --exit-char 30 --menu-char 31
```

Ctrl+^ exits that monitor; Ctrl+_ opens its menu. Other control characters,
including SolarOS's Ctrl+] app-exit key, reach the guest. Replace the environment
name with `qemu_esp32s3` for the headless target. `picocom`, `screen`, PuTTY,
and serial libraries can open the same device.

SolarOS probes terminal capabilities during boot. If you connect after boot,
type `system_reset` in the QEMU monitor while the serial terminal stays connected
so the guest can detect it. UART traffic bypasses PlatformIO's build-output
buffering; the terminal program handles rendering and keyboard input.

To bridge the emulator to a physical serial adapter on Linux, for example:

```sh
socat .pio/build/qemu_esp32s3_rgb/uart0,rawer,echo=0 /dev/ttyUSB0,rawer,echo=0,b115200
```

Use one serial client or bridge at a time. UART0 carries SolarOS's shell and boot
logs by default; configure the guest for the protocol you intend to use. The PTY
transports bytes, not electrical signals, and its baud/modem-control settings
are not propagated to a physical adapter. Set that adapter's speed in the bridge.

Native Windows uses the launching terminal for UART0; QEMU does not create PTYs
there. Use WSL for a virtual serial device. To select the original console mode
on any host, add `custom_qemu_uart0 = stdio` to the environment. In that mode,
Ctrl+A, then X exits QEMU, and Ctrl+A, then C switches to its monitor. These
shortcuts apply in the host console, independently of the graphical window.

## Host directory storage

On Linux and macOS, both targets expose the checkout's `qemu-storage` directory
at `/host` in SolarOS. The launcher creates the directory when needed and prints
the host path. It is outside `.pio`, so cleaning or rebuilding firmware does not
remove its files. Files saved under `/host` update the host directory directly;
host edits are visible when the guest reopens or refreshes them.

```text
ls /host
cd /host
python /host/demo.py
```

To share another folder, set its path in the target's `platformio.ini` environment:

```ini
custom_qemu_storage = /path/to/shared-folder
```

Relative paths are resolved from the checkout root. The mount supports files,
directories, seek, append, truncation, timestamps, rename, and filesystem usage.
The Files app lists `/host` among the available mounts. The guest's internal
flash remains a separate filesystem.

Only the selected directory tree is exposed. Symlinks inside it are not followed, and
parent traversal outside the mount is rejected. Existing SolarOS and host tools
can read and modify the same files; finish a save before opening its result in
the other system.

Set `custom_qemu_storage = none` to disable the share. Native Windows disables
it by default; use WSL or a supplied extended emulator for host storage.
`disk mount host` and `disk umount /host` control the guest mount. Unmounting
refuses open files, directories, and disk images using the share. Host directories
are not block devices and cannot be formatted with `disk format`.

### Host dependencies

Targets using native input or host storage need a C compiler, Git, pkg-config,
and development libraries to build their emulator extension once. On Ubuntu or Debian:

```sh
sudo apt-get install build-essential git python3-venv pkg-config libglib2.0-dev libpixman-1-dev libsdl2-dev libslirp-dev libgcrypt20-dev
```

On macOS:

```sh
brew install pkg-config libgcrypt glib pixman sdl2 libslirp
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

## Native input

The display target attaches the `qemu-input` device as `input0`. QEMU window
input reaches SolarOS through a memory-mapped event queue, independently of
all UART peripherals. The input service exposes three sources:

- `qemu-keyboard`: physical USB HID usages, modifiers, and key press/release.
  SolarOS applies its keyboard layout and repeat settings.
- `qemu-touch`: absolute, single-contact touch with display coordinates.
- `qemu-mouse`: relative movement, buttons, and wheel events.

Mouse-wheel ticks are Z-axis deltas on the active pointer source, which
advertises scroll capability. They are not translated into keyboard keys.
Losing window focus releases held keys and buttons. Queue overflow also
resets input state so a dropped release cannot leave a key or button held.

The default pointer mode is touch: click or drag in the window to touch the
virtual display. For a relative mouse, add this to `[env:qemu_esp32s3_rgb]`
in `platformio.ini`:

```ini
custom_qemu_pointer = mouse
```

To switch modes while running, use `info mice` in the QEMU monitor to find the
`SolarOS touch` and `SolarOS mouse` indices, and select one with `mouse_set INDEX`.
Ctrl+Alt+G releases a grabbed relative mouse.

`input status` and `input test qemu-keyboard`, `input test qemu-touch`, or
`input test qemu-mouse` inspect the sources from the UART0 console.

For an externally built emulator, set `custom_qemu_executable = /path/to/qemu-system-xtensa`
in the target environment. It must include `tools/qemu-xtensa/solar-input.patch`
and the accompanying device sources and shared protocol headers.

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

- Touch input is single-contact; multi-touch is not emulated.
- No audio, battery, Bluetooth, or SD card.
- The board declares the DevKitC-1 capabilities `gpio`, `i2c`, `spi`, `pwm`,
  `usb_host`, `key`, and their `expansion_*` forms, so the same services build
  and boot as on hardware. QEMU's ESP32-S3 machine has no device behind them:
  GPIO inputs read 0 and writes do nothing, and I2C, SPI, PWM (LEDC), I2S, and
  USB host have no peripheral to talk to. Sources: Espressif's
  [QEMU feature table](https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md)
  and `hw/xtensa/esp32s3.c` in the QEMU release above.
- Mono display only, 800x600 at most (the largest panel QEMU's `esp_rgb` model takes).
- Wi-Fi commands (`wifi scan`, `wifi connect`) have no radio to work with; the
  network comes only from `eth0`.
- No network on `qemu_esp32s3` (headless): it has no `eth0` device.
- `shutdown` on `qemu_esp32s3` prints its message but leaves the shell running.
  Likely cause, read from the code: shutdown waits for the BOOT key (GPIO0) to
  be released, and GPIO0 reads 0, the key's active level.
- Changes to the firmware's flash inputs recreate the virtual flash image,
  clearing files and settings stored in that image. Host-directory files persist.

## Quick reference

Run `pio run -e qemu_esp32s3_rgb -t qemu` for the display target, or
`pio run -e qemu_esp32s3 -t qemu` for headless. PlatformIO builds the firmware,
prepares the emulator and flash image, and starts the virtual board. Type
directly into the QEMU window for display input; connect a serial terminal to
`.pio/build/<env>/uart0` for UART0 on Linux and macOS.
