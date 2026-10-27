# Macintosh Plus foreground app

External ESP32-S3 SolarOS app based on
[epatel/esp32_mac](https://github.com/epatel/esp32_mac), revision
`eaffb010eee2267672f1f766fcc4c5eb80cb5baa`.
The Musashi 68000 core, VIA, SCC, RTC, mouse and SCSI controller run in the
SolarOS foreground worker. No Arduino, BLE server, board driver, SD mount,
flash partition or separate emulator task is initialized by this module.

The guest has 4 MiB RAM, a monochrome screen matching the selected display,
keyboard, keyboard-controlled mouse, and one optional SCSI disk or floppy. Audio and
LocalTalk are not implemented. The IWM remains a stub; floppy access uses
the replacement Sony driver described below. Physical touch and pointer
devices are not exposed by the current foreground input ABI.

## Build and install

Build with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
cmake --build build --target typed_elf
```

Copy `build/solaros_native_mac.app.elf` to `modules/apps/mac.elf` on the
preferred storage volume. The firmware must provide the foreground service
through `disk_close`. This module uses lifecycle ABI 2, a 16 KiB worker stack
and priority 1. Launch admission requires 8 KiB free internal RAM and 5 MiB
free PSRAM after loading the ELF. Guest RAM needs a contiguous 4 MiB block;
8 MiB PSRAM boards can fail admission or allocation if other apps retain
large buffers or memory is fragmented. Allocation failure returns to the shell.

```text
mac /sdcard/mac/macplus.rom
mac /sdcard/mac/macplus.rom /sdcard/mac/hd.img
mac /sdcard/mac/macplus.rom --floppy /sdcard/mac/system.img
session create mac display0 /sdcard/mac/macplus.rom --floppy /sdcard/mac/system.img
mac --self-test
```

ROM and guest disks are user-supplied and are not included. ROM loading checks
the complete 128 KiB prefix against the Mac Plus v1, v2 or v3 checksum and
reset vector. Extra trailing dump data is ignored. SCSI disks need 512-byte
sectors and a guest bootable Apple partition map and driver. A raw HFS floppy
is not a SCSI hard disk. The controller rejects transfers larger than its
32 KiB buffer and accesses past the image end.

Verified Plus v3 ROMs skip the long power-on RAM test in the in-memory copy.
The host allocates and clears guest RAM before startup.
Native display sizes other than 512×342 require Plus v3. Plus v1/v2 retain
their original screen and require a 512×342 host surface.

`--floppy` accepts one raw 400 or 800 KiB image and requires a verified Plus v3
ROM (`4d1f8172`). It patches only the in-memory ROM copy with the open source
Sony driver. The original ROM file is unchanged. Disk Copy containers and
GCR/nibble images are not accepted. Floppy and SCSI attachments are mutually
exclusive in this version.

Images are exclusively claimed through the SolarOS disk service. Guest writes
update and sync the image immediately; use copies to preserve original disks.
The image remains claimed while the app is suspended. Close the guest before
mounting its image elsewhere or removing storage. PRAM is stored in a 32-byte
`.pram` sidecar beside the ROM, on suspend and close.
Serial-port settings start in offline asynchronous mode; LocalTalk is disabled
when loading saved PRAM.

## Display, controls and sessions

The app queries the selected display dimensions and patches the in-memory Plus
v3 ROM so the guest uses that resolution. Each guest pixel maps to one display
pixel: there is no scaling, cropping or letterboxing. Both dimensions must be
64..1024 pixels; larger surfaces may exceed the available PSRAM. Rows are
word-aligned even when the visible width is not a multiple of 16 pixels.
The app checks for screen changes at up to 25 Hz and submits only changed
frames, with a complete redraw after resume. Precomputed source rows, byte
offsets and bit masks expand the packed guest pixels to display palette indices.
Software that assumes the original Mac screen size may have windows or dialogs
that do not fit smaller displays.

- Letters, punctuation and modifiers use Macintosh keyboard scan codes.
- Keypad 4/6/8/2 move the mouse; hold keypad 5 to hold its button.
- F8 toggles mouse mode: arrows move and Space holds the mouse button.
- F2 saves the guest framebuffer as a `.pbm` sidecar beside the ROM.
- F10 toggles pause.
- Ctrl+] closes the app. SolarOS session shortcuts retain their normal use.

On character-only input sources, each arrow event moves six pixels and Space
toggles the button because those sources do not supply key-release events.

Suspend retains CPU, RAM, peripheral state, pause state and image claims.
It releases display and input ownership. Resume clears held inputs and
submits a complete frame. The guest has no callbacks or workers that survive
module unload. CPU batches are bounded and yield to the host scheduler.

`--self-test` runs an original diagnostic ROM that fills the guest framebuffer
with stripes and increments a RAM counter. It needs no Apple files and does
not boot Macintosh software.

## Source and licenses

The original Beer-Ware notices and Musashi MIT permission text are retained
under `main/vendor`. SolarOS changes provide checked memory accesses, bounded
execution, deferred device interrupts, recoverable failures, controller bounds,
input translation and foreground lifecycle handling.

The replacement Sony driver comes from
[umac](https://github.com/evansm7/umac), revision
`b62d3c6e725ca2ac2224e710b3cee765dbc35e45`, and its Basilisk II driver code by
Christian Bauer. Its GPL notices, Matt Evans's notices, corresponding assembly
source and generated bytes are retained under `main/vendor/sony`; see `COPYING`.
SolarOS changes bound guest-memory spans, disk offsets, drive-queue traversal
and transfer chunks, and update actual transfer counts.
Plus v3 installs this driver even without a floppy image. Native-resolution
patches in `main/video.c` derive from umac's MIT-licensed `rom.c` and
[Mini vMac](https://github.com/minivmac/minivmac)'s GPL-2.0 `SCRNHACK.h` by
Paul C. Pratt; their notices are retained in that file.

To regenerate the bundled Sony stub with GNU m68k binutils:

```sh
cpp main/vendor/sony/sonydrv.S | m68k-linux-gnu-as -o /tmp/sonydrv.o
m68k-linux-gnu-ld /tmp/sonydrv.o -o /tmp/sonydrv.elf -Ttext=0
m68k-linux-gnu-objcopy /tmp/sonydrv.elf -O binary --only-section=.text /tmp/sonydrv.bin
xxd -i < /tmp/sonydrv.bin > main/vendor/sony/sonydrv.h
```

No Apple firmware, operating system, or disk image is part of these sources.
