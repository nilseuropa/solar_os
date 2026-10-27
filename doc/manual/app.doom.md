+++
id = "app.doom"
title = "DOOM application"
section = "app"
summary = "Run DOOM IWAD files with the external PrBoom foreground app"
aliases = ["doom"]
keywords = "doom prboom iwads wad game keyboard suspend resume"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# DOOM

DOOM is an external foreground app for ESP32-S3 devices with PSRAM, graphics,
and native-module support. It uses Espressif's PrBoom port. Game data is supplied
separately as a DOOM or DOOM II IWAD. Music, networking, savegames, persistent
configuration, PWAD overlays and DeHackEd patches are unsupported.

## Quick reference

```text
doom /sdcard/doom/doom1.wad
session create doom display0 /sdcard/doom/doom1.wad
load /sdcard/solaros_native_doom.app.elf /sdcard/doom/doom1.wad
session close ID
```

## Installation and launch

Copy `solaros_native_doom.app.elf` to `modules/apps/doom.elf` in the preferred
storage volume. The app requires at least 4 MiB free PSRAM and a 24 KiB worker
stack. Large WADs or other resident apps can exhaust available memory.

Supply exactly one IWAD path. Keep its storage mounted and the file unchanged
until the app closes. WAD files are read through SolarOS storage services;
the app does not require a dedicated flash partition. The game renders at
320x240 with a fixed RGB332 output palette and uses the host's display fitting
and monochrome conversion.

## Sound effects

Sound effects play through the selected SolarOS audio output, using the global
speaker level. The game's sound-volume setting controls the effects mix. Busy
or unavailable audio output leaves the game running silently.

The IWAD must contain digital sound-effect lumps (`DS*`). Espressif's
`doom1-cut.wad` has no sound-effect data; use an uncut shareware or owned
full-game IWAD for sound. Music is disabled. Session suspension stops audio;
resume reopens the output and continues the retained sample positions.

## Controls and sessions

| Key | Action |
| --- | --- |
| Arrow keys | Move forward/backward and turn |
| Ctrl | Fire |
| Space | Open doors and use switches |
| Shift | Run |
| Alt | Strafe with arrow keys |
| Comma / period | Strafe left / right |
| 1–7 | Select weapon |
| Tab | Toggle map |
| Escape | Open menu |
| Enter | Select menu item |

Held movement requires a keyboard source that sends key releases. Text-only
sources provide taps. Normal SolarOS session shortcuts suspend the game while
retaining its state. Resume clears held keys and continues the game clock from
where it paused. Ctrl+] or `session close ID` closes the app; progress is not
saved to disk.
