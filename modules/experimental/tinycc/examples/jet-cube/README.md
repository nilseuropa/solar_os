# Native C graphics with Jet

On an ESP32-S3 board with PSRAM, a display and the current package service:

```
pkg install cc
pkg install jet
cc main.c -o /modules/apps/jet-cube.elf
```

Place `main.c` in a writable directory on the device. It can be changed with
`edit main.c`. Compilation reads `<jet/solar_os_jet_abi.h>` and its included
headers directly from the installed Jet library; no separate SDK directory is
needed. `pkg install cc` also installs its exact TinyCC library dependency.

Launch `jet-cube` from the display shell. From a network shell, use:

```
session create jet-cube display0
```

Arrows rotate the cube, Space pauses or resumes animation, and Q or Escape
exits. `jet-cube --paused` starts with a stationary cube. Starting another
foreground session suspends the cube; closing that session restores it.

This example uses the versioned foreground and library services. Jet renders
MSB-first RGB565 into borrowed buffers. The example converts those pixels to a
grayscale palette for the foreground presentation API. It keeps the provider
lease until its scene is destroyed, polls cancellation, and frees its buffers
before returning.
