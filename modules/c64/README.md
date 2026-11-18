# Commodore 64 foreground app

PAL C64 emulator for SolarOS on ESP32-S3, using the CPU6502, VIC-II, CIA
and SID cores from [retroelec/T-HMI-C64](https://github.com/retroelec/T-HMI-C64).
The module includes the standard BASIC, KERNAL and character ROMs, so `c64`
boots directly to BASIC without external ROM files. `--roms DIRECTORY`
selects an alternate set instead.

See the [c64 manual](../../doc/manual/app.c64.md) for installation, ROMs,
program loading, controls and compatibility.

## Build

Build with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
cmake --build build --target typed_elf
```

The artifact is `build/solaros_native_c64.app.elf`. Copy it to
`modules/apps/c64.elf` on the preferred storage volume. The module manifest
is discovered by the standard module catalog builder; no firmware package
or board driver is added.

Requires lifecycle ABI 2, the foreground service through `video_dimensions`,
and the base memory service through `free`. Frame exchange and joystick axes
are optional appended services. The app declares a 16 KiB worker stack,
8 KiB free internal memory and 2 MiB free PSRAM after ELF loading. Host
allocation admission can still refuse a display buffer.

CPU and chipset work stays on the foreground worker. SolarOS owns display
presentation, keyboard/button/axis routing, paths, file access, timing and
audio output. A bounded single-producer/single-consumer PCM queue separates
SID generation from the host audio callback. Close joins the callback before
guest allocations are released. Suspend retains guest RAM, chip state and
loaded media, clears held input and rebases pacing on resume.
The adapter queries the selected output's sample rate before opening the
synth, and converts the core's 16 kHz PCM to that rate in the callback.
An overlap/add clock follower matches uneven guest production while preserving
waveform pitch, with 128 ms of prebuffering and fades on underrun.
The CPU uses direct C64 bus calls, and hot state and RAM use internal SRAM
when the host can preserve its memory reserve. On 320-pixel-wide displays
that fit the active picture, VIC renders into exchanged host buffers directly;
other sizes use the fitted picture path.

## Validation

From the firmware repository root:

```sh
python3 tests/test_c64_module.py
```

The host suite uses ASan/UBSan and original diagnostic ROM code. It checks
CPU execution, banking, CIA keyboard scanning in both directions, multiple
held keys, joystick axes, VIC modes and sprites, SID PCM, media bounds,
display geometry, frame exchange, suspend/resume and allocation/error cleanup.
It also checks that the bundled ROMs boot to `READY.` without file access.
No external ROM downloads or files are needed.

On a device, `c64 --self-test` runs the original diagnostic guest without
ROM or program files. This exercises CPU, video and optional audio through
the normal session lifecycle. It is not a substitute for testing licensed
software or measuring emulation speed on a particular board.

For automated device checks, `c64 --self-test /sdcard/c64/diagnostic.ppm`
enables F2 snapshots in diagnostic mode only. It writes a 320×200 PPM and
an 80-byte little-endian `.state` sidecar: `C64S`, version at offset 4,
guest frames at 8, PC at 12, counter at 16, pause/audio/joystick mode at
20/24/28, keyboard rows at 32, joystick ports and guest CIA samples at 40,
elapsed milliseconds at 44, presenter frames/replacements at 48/52,
presenter microseconds at 56, PCM write/read indices at 64/68, underrun
events at 72 and output frames at 76.
Normal C64 software retains F2 as a guest function key.
For ROM-based validation, `--snapshot /sdcard/c64/guest.ppm` explicitly
reserves F2 for the same capture without replacing the guest ROMs.

Run the device acceptance suite on an idle display with stopped `displayd`
and `ftpd` jobs:

```sh
python3 tests/hil/c64_module.py --host DEVICE_ADDRESS \
  --elf modules/c64/build/solaros_native_c64.app.elf \
  --output /tmp/c64-hil --require-audio
```

This installs and reads back the ELF, checks guest frames and CIA input,
pause and retained suspension, audio ownership, cleanup and repeated relaunch.
It leaves the installed ELF and diagnostic snapshots on the SD card.

## Source and license

Upstream revision: `fce5a1d8bf1deba34f8050175bbfcb6969db1b32`.
Upstream core files carry GPL-3.0-or-later notices. The license text is in
`main/vendor/COPYING`; the SolarOS adapter is also GPL-3.0-or-later.

Vendored from upstream `src/`:

- `CPU6502.{cpp,h}`: CPU core.
- `CIA.{cpp,h}`: timers and time-of-day clock.
- `VIC.{cpp,h}`: raster renderer, modes, sprites and collision detection.
- `SID.{cpp,h}`: voices and sample generation.
- `C64Sys.{cpp,h}`: memory bus and raster execution, reduced to a bounded core.

Local changes remove hardware factories, permanent tasks, upstream ROM embedding,
overlays, BLE/web input, network/OTA, filesystem drivers and the floppy/UI
layer. VIC buffers are caller owned and expanded sprites clip each output
pixel. SID fills exactly 320 samples per PAL frame at 16 kHz, avoiding the
upstream per-raster buffer overrun, retains oscillator phase on wrapping and
uses the sustain register's high nibble. The memory bus uses
a full keyboard matrix, honors the CPU port direction register, masks color
RAM and converts the SID envelope after scaling. No game images are included.
D64/1541, cartridges, tape and in-guest file I/O are not implemented.

The bundled Commodore ROMs are separate from the GPL emulator code and retain
their original Commodore copyright. [ROM provenance](rom/README.md) records
the Zimmers source files and pinned SHA-256 digests. Build generation checks
each file's size and hash and embeds it as read-only module data; the build
does not download assets. The default guest reads these arrays directly,
without allocating a second ROM copy.
