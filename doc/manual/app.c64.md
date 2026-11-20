+++
id = "app.c64"
title = "c64 application"
section = "app"
summary = "Run Commodore 64 software with the external foreground module"
aliases = ["c64", "commodore64"]
keywords = "commodore 64 emulator pal 6510 vic cia sid prg keyboard joystick suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# c64

`c64` emulates a PAL Commodore 64 with 64 KiB RAM, VIC-II graphics, CIA
timers, keyboard, joystick and SID audio. It requires an ESP32-S3 board
with graphics, PSRAM and native-module support.

## Quick reference

Check `pkg available` for a compatible package, then use `pkg install c64`.
For manual installation, copy `solaros_native_c64.app.elf` to
`modules/apps/c64.elf` on the preferred storage volume. Launch requires at
least 2 MiB free PSRAM after ELF loading.

The module includes BASIC V2 (`901226-01`), KERNAL revision 3 (`901227-03`)
and the standard character ROM (`901225-01`). No separate ROM files are
needed. To use alternate ROMs, select their directory with `--roms`; it must
contain these raw files:

| File | Size |
| --- | --- |
| `basic.rom` | 8192 bytes |
| `kernal.rom` | 8192 bytes |
| `chargen.rom` | 4096 bytes |

Program files are user supplied. External ROM and program files are never modified.

```text
cd /sdcard/c64
c64
c64 game.prg
c64 --roms /sdcard/c64 --joystick 2 /sdcard/games/game.prg
```

Paths can be absolute or relative to the launching shell's current directory.
Opening a `.prg` file launches `c64` using the built-in ROMs.

From a port shell, select the display explicitly:

```text
session create c64 display0 --roms /sdcard/c64 /sdcard/games/game.prg
session close ID
```

## Programs

Without a program argument, the guest boots to BASIC. PRG files include
their two-byte little-endian load address. After the standard KERNAL reaches
its keyboard input loop, the module copies the program into guest RAM.
Programs loaded at `$0801` run through BASIC `RUN`; other programs start
at their load address. Machine-code programs that need a different entry
point require an appropriate BASIC loader.

The loader rejects data that extends beyond guest RAM. It does not provide
a floppy drive or intercept BASIC `LOAD` and `SAVE` commands.

## Display and controls

The C64 has a fixed 320×200 active picture. It is centered at 1:1 or an
integer enlargement where it fits, with the guest border color around it.
Smaller displays use proportional nearest-neighbor reduction.

| Control | Action |
| --- | --- |
| Keyboard | C64 keyboard; default mode |
| F1–F8 | C64 function keys; even keys use guest Shift |
| Arrows | C64 cursor keys in keyboard mode |
| Esc | Run/Stop |
| Backspace / Delete | C64 Delete |
| Home | C64 Home |
| Ctrl | C64 Control |
| Alt | Commodore key |
| F9 | Cycle keyboard joystick port 2, port 1, then keyboard mode |
| Arrows + Space | Direction and fire in keyboard joystick mode |
| F10 | Pause / continue |
| F11 | Cold reset to BASIC; does not reload the PRG |
| F12 | Restore |
| Ctrl+] | Close |

`--joystick 1` or `--joystick 2` starts in keyboard joystick mode. Board
direction buttons and the first connected USB joystick use port 2 in keyboard
mode, or the selected joystick port. USB joystick X/Y axes control direction;
buttons 1–4 fire. Board Space/Enter buttons also fire. Keyboard and joystick
inputs clear when the session switches or an input queue resets.

Audio is optional and uses the SolarOS volume setting. Session switching
suspends the machine and retains guest state; closing releases it.

## Compatibility

The module uses the CPU, video, timer and sound cores from
[T-HMI-C64](https://github.com/retroelec/T-HMI-C64). It does not include the
upstream Android/BLE keyboard, web keyboard, Wi-Fi setup or board firmware.
D64/1541 drives, cartridges, tape, saved machine states, disk writes and cycle-exact
timing are not supported. SID synthesis does not emulate analog filters.
PRG autostart expects the standard C64 BASIC/KERNAL entry points; replacement
ROMs may boot interactively but may not support automatic program loading.

`c64 --self-test` runs an original diagnostic guest without ROMs or software.
To inspect guest output, `--snapshot /sdcard/c64/guest.ppm` reserves F2 for
a 320×200 PPM capture and a `.state` sidecar. Without this option, F2 remains
a C64 function key.
See [Native foreground apps](native-app.md) for installation and sessions.
