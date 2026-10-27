+++
id = "app.amiga"
title = "amiga application"
section = "app"
summary = "Run Amiga 500 software with the external foreground module"
aliases = ["amiga", "amiga500"]
keywords = "amiga 500 emulator kickstart adf floppy ocs paula mouse joystick fitted PAL display suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# amiga

`amiga` emulates an Amiga 500 with 1 MiB RAM, keyboard, mouse, joystick,
audio and one read-only floppy drive. It requires an ESP32-S3 board with
PSRAM, graphics and native-module support.

## Quick reference

Copy `solaros_native_amiga.app.elf` to `modules/apps/amiga.elf` on the preferred
storage volume. Boards with 8 MiB PSRAM are recommended; the app needs at
least 3 MiB free PSRAM after loading.

```text
cd /sdcard/amiga
amiga kick13.rom workbench.adf
```

Supply your own raw 256 KiB Kickstart 1.3 ROM and, optionally, an 880 KiB
AmigaDOS ADF image. Omit the ADF argument to start without a disk. Neither
file is included, and disk images are not modified.

Paths can be absolute or relative to the current directory. Tab completes
both filenames.

To launch on the display from a port shell:

```text
session create amiga display0 /sdcard/amiga/kick13.rom /sdcard/amiga/workbench.adf
session close ID
```

## Display and controls

The Amiga screen fits the display with black borders where needed.

| Control | Action |
| --- | --- |
| Keyboard | Type into the guest; default mode |
| F6 | Select keyboard mode (default) |
| F7 | Select keyboard mouse emulation |
| F8 | Select keyboard joystick emulation |
| Arrows | Guest arrows, mouse movement or joystick direction by mode |
| Space | Guest Space, left mouse button or joystick fire by mode |
| Enter | Guest Return; right mouse button in mouse/joystick modes |
| F2 | Save a PPM snapshot beside the ROM (`.ppm`) |
| F10 | Pause / continue |
| Ctrl+] | Close |

Mouse and touch input work in every mode. On touchscreens, slide to move,
tap to click, or hold still briefly and then slide to drag. Lift to release.

Audio uses the SolarOS volume setting when audio hardware is available.
Switch sessions to suspend and resume emulation; close the session to stop it.

## Compatibility

The app supports standard AmigaDOS floppy images. Disk swapping, hard disks,
networking, HAM, dual playfield, interlace, AGA and overscan are not supported.
Programs that require precise Copper timing may display incorrectly.

`amiga --self-test` checks the emulator without ROM or disk files.
See [Native foreground apps](native-app.md) for module installation and sessions.
