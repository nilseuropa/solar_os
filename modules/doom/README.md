# DOOM foreground app

External SolarOS ESP32-S3 app based on
[Espressif's esp32-doom](https://github.com/espressif/esp32-doom) revision
`085f21b630f11adb69a3894db5a1efa39a7aa31a` (PrBoom 2.5.0).
The engine, tables, authors and GPL license are retained in `main/vendor`.
The SolarOS adapter is GPL-2.0-or-later. No game WAD is bundled.

## Build and launch

Build with ESP-IDF 5.5 for ESP32-S3, from this directory:

```sh
idf.py set-target esp32s3
cmake --build build --target typed_elf
```

Copy `build/solaros_native_doom.app.elf` to
`/sdcard/modules/apps/doom.elf`, and supply a DOOM or DOOM II IWAD:

```text
doom /sdcard/doom/doom1.wad
load /sdcard/solaros_native_doom.app.elf /sdcard/doom/doom1.wad
session create doom display0 /sdcard/doom/doom1.wad
```

Only one IWAD argument is accepted. PWAD overlays, DeHackEd patches, networking,
music, savegames and persistent configuration are unsupported. The WAD remains
on storage; keep it mounted and unchanged while the app is running or suspended.
The game uses a 320x240 indexed frame, fitted by the host presenter. Palette
flashes are converted to a fixed RGB332 palette. Monochrome output uses the
host's display conversion. Missing PrBoom color tables use identity mappings;
missing alternate HUD artwork uses the IWAD's original font.
Vanilla IWADs use the standard engine switch and animation definitions when
the Boom `SWITCHES` and `ANIMATED` tables are absent.

Sampled sound effects use the selected SolarOS audio output and global speaker
level. The module mixes the engine's sound channels into stereo PCM, with
resampling, distance attenuation and stereo positioning. The game's sound
volume controls its mixer gain. An unavailable or busy audio output leaves the
game usable silently. The IWAD must contain `DS*` digital sound lumps; Espressif's
`doom1-cut.wad` omits them. Use an uncut shareware or owned full-game IWAD for
sound effects. Music remains disabled.

Controls: arrow keys move/turn, Ctrl fires, Space uses doors/switches, Shift runs,
Alt strafes, comma/period strafe left/right, 1–7 select weapons, Tab opens the map,
Escape opens the menu, and Enter selects. Use a keyboard that supplies key
releases for held actions. Text-only key sources provide taps.

SolarOS session shortcuts suspend the game. Resume retains game state, clears
held keys, excludes suspension time from the game clock, and redraws the frame.
Ctrl+] or `session close ID` closes it and releases all module allocations.

If a replacement ELF changes the app's resource declaration, reboot SolarOS
before launching it. App discovery retains that declaration until restart.

The manifest declares a resumable GUI app, a 24 KiB worker stack, priority 1,
8 KiB free internal memory and 4 MiB free PSRAM minimum. These are launch
admission checks, not reserved memory or a guarantee that every IWAD fits.
The adapter uses only versioned SolarOS services and initializes no peripherals.
Its PCM callback runs on the host audio worker and is joined before suspension
or unload. Resume reopens the selected output and continues retained samples.

## Source and validation

`main/vendor/prboom` and `main/vendor/tables` come from the revision above.
`i_main.c` retains upstream platform startup; `i_sound.c` supplies the SolarOS
sampled-effects backend. Local engine changes replace raw zone allocation, preserve
integer booleans with SolarOS headers, remove hardware/IRAM headers, and supply
fallbacks for optional PrBoom artwork and colormap markers, standard switch and
animation tables, and cooperative checks during translucency-map generation.
The table defaults reproduce the engine definitions in upstream's cut WAD.
The adapter supplies tracked PSRAM,
bounded read-only WAD access with an unlocked-lump cache, fixed-palette video,
keyboard events, time, cooperative lifecycle checks and trapped engine exits.
The DeHackEd translation unit supplies engine string defaults; patch processing
is rejected by the adapter. Process-wide stdio and configuration access are
disabled. Pure string helpers and libm are linked into the module.

Run the host tests from the SolarOS repository root:

```sh
python3 -m unittest discover -s tests -p test_doom_module.py
SOLAR_OS_DOOM_TEST_WAD=/path/to/doom1.wad python3 -m unittest discover -s tests -p test_doom_module.py
```

The optional IWAD fixture must contain Episode 1 and standard DOOM menus; digital
sound lumps enable the audio integration cases. The host checks exercise
gameplay, keyboard input, suspend/resume, close, sample mixing, resampling,
malformed samples and directories, short reads, allocation failures and cleanup.
Hardware frame rate,
display behavior and memory availability depend on the selected device.
