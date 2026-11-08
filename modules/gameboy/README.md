# Game Boy foreground app

External SolarOS app using the vendored Peanut-GB CPU core, ROM validator,
packed video conversion and MiniGB APU. The original licenses are in
`main/vendor/peanut_gb`. No ROM images are bundled.

Build with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
idf.py build
cmake --build build --target typed_elf
```

Artifact: `build/solaros_native_gameboy.app.elf`. Install from a compatible
catalog with `pkg install gameboy`, or copy the artifact to
`/sdcard/modules/apps/gameboy.elf`. Launch from a display shell:

```text
gameboy /sdcard/roms/game.gb
```

You can also use
`session create gameboy display0 /sdcard/roms/game.gb` or
`load /sdcard/gameboy.app.elf /sdcard/roms/game.gb`.

Files opens `.gb` ROMs through the installed app's file association.

Arrow keys control the directional pad. The physical US-Z key position and X
are A and B; the US-Z key is labeled Y on German QWERTZ keyboards. Enter is
Start, and Backspace/Delete is Select. Character-only input pulses buttons for
140 ms. P pauses, R resets, and Q, Escape or Ctrl+] exits. On boards with a
buttons input source, Enter is A, Space is B, the app-exit button is Start and
Escape is Select; pressing Start and Select together exits.

The first ready joystick is used automatically, including when connected after
launch. X/Y control directions with a dead zone (engage at 10000, release at
7000 on the normalized -32767..32767 range). Hats/D-pads also control directions.
Buttons 1/3 are A, 2/4 are B, 9 is Select, and 10 is Start; pressing joystick
Start and Select together exits. Unplugging releases its controls. Keyboard
and board controls remain available. This module requires host firmware with
native axis-event support.

Session switching retains guest CPU, RAM, cartridge RAM, pause state and APU
state. Display and audio are released for the other foreground app. Resume
clears held inputs and rebases frame timing. Closing writes modified cartridge
RAM to a `.sav` file alongside the ROM using atomic replacement. Existing saves
are loaded on launch; a size mismatch is reported. Reset preserves cartridge RAM.

The module accepts original Game Boy ROMs up to 4 MiB. Color-only ROMs and
unsupported cartridge controllers are rejected. Video uses packed INDEX2 frames
at up to 25 presented frames/s. The guest frame period is 16743 microseconds,
with at most three catch-up frames. Audio uses MiniGB at 16 kHz stereo with gain
4 through the host synth service; unavailable audio leaves emulation running.

The foreground manifest declares an 8 KiB worker, priority 2, 16 KiB of internal
memory and 256 KiB of PSRAM for admission. ROM and cartridge allocations are
checked at launch. Hot core/APU state prefers internal RAM and can fall back to
PSRAM. Two optional 16 KiB internal ROM caches preserve the OS reserve.
See the [foreground app contract](../../doc/manual/native-app.md).
The [Game Boy user guide](../../doc/manual/app.gameboy.md) covers installation,
controls, and save files.
