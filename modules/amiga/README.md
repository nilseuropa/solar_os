# amiga foreground app

Amiga 500 emulator for SolarOS on ESP32-S3, based on
[Makerstown/amiga500-esp32](https://github.com/Makerstown/amiga500-esp32).
It provides 1 MiB RAM, keyboard, mouse, joystick, audio and one read-only
AmigaDOS floppy drive.

See the [amiga manual](../../doc/manual/app.amiga.md) for launch commands,
controls, supported images and limitations.

## Build and install

Build from this directory with ESP-IDF 5.5 for ESP32-S3:

```sh
idf.py set-target esp32s3
cmake --build build --target typed_elf
```

Copy `build/solaros_native_amiga.app.elf` to `modules/apps/amiga.elf` on the
preferred storage volume. The launch command is `amiga`.

The firmware must support lifecycle ABI 2, the pointer-events manifest flag
and the foreground service through `calendar_read`. Boards with 8 MiB PSRAM
are recommended. Launch requires at least 3 MiB free PSRAM and 8 KiB free
internal RAM after ELF loading, plus a 16 KiB worker stack.

Supply your own raw 256 KiB Kickstart 1.3 ROM and an optional 880 KiB
AmigaDOS ADF image. These files are not included or modified.

```text
cd /sdcard/amiga
amiga kick13.rom workbench.adf
```

## Checks

Run the host tests from the SolarOS repository root:

```sh
python3 tests/test_amiga_module.py
```

Run the device checks on an idle display:

```sh
python3 tests/hil/amiga_module.py --host DEVICE_ADDRESS \
  --elf modules/amiga/build/solaros_native_amiga.app.elf \
  --size WIDTH HEIGHT --output /tmp/amiga-hil
```

The device check installs the module and uses its built-in diagnostic;
no ROM or disk image is required. To run the diagnostic directly:

```text
amiga --self-test
amiga --self-test /sdcard/amiga/diagnostic.ppm
```

## Source and licenses

Upstream revision: `ba0f6f10efde64abcd07c217ba46b5506dcc7a4b`.
The chipset sources come from `esp32`, with matching Musashi sources from
`pc/musashi` and the ESP32 68000 configuration.

The upstream MIT license is retained in `main/vendor/LICENSE`. Musashi and
SoftFloat license notices remain in their source files.
