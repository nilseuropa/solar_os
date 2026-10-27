+++
id = "app.mac"
title = "Macintosh Plus application"
section = "app"
summary = "Run Macintosh Plus software with the external foreground module"
aliases = ["mac", "macintosh"]
keywords = "mac macintosh plus emulator rom scsi floppy disk keyboard mouse suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Macintosh Plus

Mac is an external foreground app for ESP32-S3 boards with PSRAM, graphics
and native-module support. It emulates a Macintosh Plus with 4 MiB RAM,
a monochrome screen matching the selected display, keyboard, mouse and one disk image.
Audio, networking and physical pointer input are not supported.

## Quick reference

Copy `solaros_native_mac.app.elf` to `modules/apps/mac.elf` on the preferred
storage volume. The app requires at least 5 MiB free PSRAM after loading its
ELF, including a contiguous 4 MiB guest RAM allocation, and a 16 KiB worker
stack. Close other large apps if allocation fails.

```text
mac /sdcard/mac/macplus.rom
mac /sdcard/mac/macplus.rom /sdcard/mac/hd.img
mac /sdcard/mac/macplus.rom --floppy /sdcard/mac/system.img
session create mac display0 /sdcard/mac/macplus.rom --floppy /sdcard/mac/system.img
session close ID
```

Supply your own Macintosh Plus ROM and guest disks. A ROM must start with a
valid 128 KiB Plus v1, v2 or v3 image. Trailing dump data is ignored after
checksum validation. Without a disk, the guest waits for a boot disk.
Plus v3 skips its long power-on RAM test after the host clears guest RAM.

The optional SCSI hard disk uses 512-byte sectors and needs a bootable Apple
partition map and disk driver. `--floppy` instead accepts a raw 400 or 800 KiB
floppy and requires a Plus v3 ROM. The app patches its in-memory ROM copy
with a replacement Sony driver. Disk Copy containers and nibble images are
not supported. Only one image can be attached in this version.

## Display and controls

The app queries the selected display and adapts the guest resolution, mapping
one guest pixel to one display pixel without scaling or cropping. This requires
a Plus v3 ROM for resolutions other than 512×342. Display dimensions must each
be 64..1024 pixels, subject to available PSRAM. The ROM file is unchanged.
Software with fixed-size windows or dialogs may not fit smaller displays.
The app checks for screen changes up to 25 times per second and redraws only
changed frames, plus a complete frame after resume.
Keyboard letters, punctuation and modifiers go to the guest.

| Control | Action |
| --- | --- |
| Keypad 4, 6, 8, 2 | Move the mouse left, right, up, down |
| Keypad 5 | Hold the mouse button |
| F8 | Toggle arrows-and-Space mouse mode |
| Arrows, Space in mouse mode | Move the mouse, hold its button |
| F2 | Save a guest screen snapshot beside the ROM as `.pbm` |
| F10 | Pause or continue the guest |
| Ctrl+] | Close the app |

Character-only input sources move six pixels per arrow event; Space toggles
the button because those sources do not send key releases.

`mac --self-test` runs a striped diagnostic screen without a Macintosh ROM or
disk. It checks the emulator path and does not run Macintosh software.

## Sessions and storage

SolarOS session shortcuts suspend and resume the guest. CPU, RAM, peripheral
state, pause state and disk claims remain resident while suspended. Resume
clears held input and redraws the screen.

Guest disk writes are saved and synced immediately. Use a copy of an image
to preserve the original. A suspended guest still owns its image. Close it
before mounting that image elsewhere or removing storage. PRAM is saved in a
32-byte `.pram` file beside the ROM when the app suspends or closes.
Serial-port settings use offline mode because LocalTalk is not implemented.

See the [foreground module contract](native-app.md) and
[module sources and build instructions](https://github.com/nilseuropa/solar_os/blob/main/modules/mac/README.md).
