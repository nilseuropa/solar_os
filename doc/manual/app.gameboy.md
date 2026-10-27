+++
id = "app.gameboy"
title = "Game Boy application"
section = "app"
summary = "Run original Game Boy ROMs with the external foreground module"
aliases = ["gameboy"]
keywords = "gameboy emulator rom cartridge save pause resume audio"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# Game Boy

Game Boy is an external foreground app for ESP32-S3 devices with PSRAM,
graphics, and native-module support. It runs original Game Boy (DMG) ROMs up to
4 MiB. Game Boy Color-only ROMs and unsupported cartridge controllers are
rejected. No ROM images are bundled.

## Quick reference

```text
pkg install gameboy
gameboy /sdcard/roms/game.gb
session create gameboy display0 /sdcard/roms/game.gb
```

## Installation and launch

Install from a compatible module catalog with `pkg install gameboy`, or copy
`solaros_native_gameboy.app.elf` to `modules/apps/gameboy.elf` in the preferred
storage volume. The app appears in the installed app registry and handles `.gb`
files opened from Files. ROM paths can be relative to the launching shell's
current directory.

You can also launch an ELF directly:

```text
load /sdcard/gameboy.app.elf /sdcard/roms/game.gb
```

Video uses a four-shade 160x144 image, scaled to the display. The guest runs at
its native frame frequency; presentation is bounded to 25 frames per second.
Audio uses the shared synth service at 16 kHz stereo when available. Emulation
continues without audio if the firmware omits synth support or no compatible
speaker output is available.

## Controls

- Arrows control the D-pad.
- The physical US-Z key position is A; on German QWERTZ keyboards it is labeled
  `Y`. The physical X key is B.
- `Enter` is Start; `Backspace` or `Delete` is Select.
- `p` pauses and `r` resets.
- `q`, `Esc`, or the app-exit key exits.
- For board-button input, Enter is A, Space is B, the app-exit button is Start,
  and Escape is Select. Pressing Start and Select together exits.

Held-key input supports combinations such as diagonals and direction plus A/B.
Character-only input uses short button pulses. Analog joystick axes do not
become Game Boy keys.

## Sessions and saves

Session switching retains guest CPU, RAM, cartridge RAM, pause state, and audio
synthesizer state. Display and audio are released while suspended. Resume clears
held inputs and rebases guest timing. Closing writes modified cartridge RAM
to a `.sav` file beside the ROM using atomic replacement. Existing saves are
loaded on launch; a size mismatch is reported. Reset preserves cartridge RAM.

See the [foreground app contract](native-app.md) and the
[module build instructions](https://github.com/nilseuropa/solar_os/blob/main/modules/gameboy/README.md).
