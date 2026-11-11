+++
id = "flash"
title = "Flash another ESP board"
section = "app"
summary = "Download verified SolarOS factory images and program another ESP board over UART or hosted USB serial"
aliases = ["device-flasher", "factory-flash"]
keywords = "flash factory firmware uart boot reset esp32 serial catalog sd usb pendrive cache"
packages_any = ["app_flash"]
+++
# Flash another ESP board

`flash` downloads complete SolarOS factory images to a mounted SD card or USB
drive and programs another
supported ESP board through a named UART bus or hosted USB serial port.
For UART wiring, the target must share ground with
the SolarOS device. Connect the SolarOS UART TX pin to target RX and SolarOS RX
to target TX. Both sides use 3.3 V logic; do not connect a 5 V UART signal.

Open the catalog browser:

```text
flash
```

The browser reads only the verified catalog saved on the selected cache volume. It does not access the
network when it opens. Its Catalog tab presents the available board, flavor,
and version entries as a foldable tree. All groups are initially folded. Use Up
and Down to select an entry, Left and Right to fold or unfold a group, and
Enter to act on a version. The browser retains the selected entry, scroll
position, and folded groups after catalog refreshes and artifact operations.
An asterisk marks an artifact that is cached on SD. Enter offers to download a
remote artifact or program a cached artifact. `d` always offers a download,
`f` always offers programming, and `r` refreshes the signed catalog. Delete or
`x` asks for confirmation before it removes the selected cached artifact from
the cache volume. The verified catalog entry stays available, so the artifact can be
downloaded again.

Press Tab to open Settings. Select the serial port with Left and Right. Connected USB serial ports appear
alongside UART buses. Select
BOOT pin or RESET pin and press Enter to enter a GPIO number from 0 to 63. An
empty value disables automatic control; Delete also clears the selected pin.
These settings apply to the current Flash app session. The control pins refer
to GPIOs on the SolarOS device, not pins on the target.

Before programming starts, a popup stays visible with the instructions for the
current control-pin setup. With UART and no control pins, put the target into ROM download mode manually
while the popup is open. USB serial uses DTR/RTS when no GPIO pins are selected. During refresh, download,
verification, and programming, the popup shows the current stage and progress.
The final success or failure remains visible until it is dismissed.

Catalog refresh uses the same repository as `ota url`. For example, an OTA URL
of `http://server/solaros/latest` reads the flash catalog and signature from
`http://server/solaros/flash/`. Artifact URLs then come from the verified
catalog.

The same operations are available from the shell:

```text
flash refresh
flash list
flash download BOARD FLAVOR [VERSION]
flash BOARD FLAVOR [version=VERSION] [port=uart0] [boot=PIN] [reset=PIN] [baud=RATE]
```

Shell completion for `flash BOARD FLAVOR` reads the verified cached catalog and
offers only boards and flavors whose factory artifact is already cached.

When `version` is omitted, SolarOS selects the first matching catalog entry;
the release catalog orders newer versions first. The program command refuses
to use an artifact that is not already cached. `port`, `boot`, and `reset`
refer to resources on the SolarOS device, not pins on the target.

If both `boot` and `reset` are present, SolarOS drives the target into ROM
download mode automatically. With only `boot`, SolarOS holds that signal low
while it starts the connection; reset the target manually. With only `reset`,
hold the target's boot strap active while SolarOS toggles reset. With neither on a UART port, put the target in download mode before you run
the command. USB serial uses DTR/RTS as described below.

## Flash over USB serial

On a USB-host-capable SolarOS device with `service.usb-serial`, connect the
target's native USB Serial/JTAG port or a supported CP210x/CH340 USB–UART
bridge, then select the detected port:

```text
usb mode host
usb serial
flash BOARD FLAVOR port=usb-serial0
```

Use the exact board/flavor from `flash list` after downloading its factory
image. With no `boot=` or `reset=` GPIOs, Flash uses USB DTR/RTS for the native
Serial/JTAG or bridge reset sequence. A USB–UART bridge needs the target's
usual automatic BOOT/RESET circuitry. With external GPIO pins configured,
Flash uses the same GPIO control rules as UART.

For targets whose running firmware exposes USB CDC, enter ROM download mode
with BOOT/RESET first, wait for the port to reconnect, and select its current
name. The flash operation does not follow USB re-enumeration: a disconnect
aborts it. Close `com` or any script holding that port before programming.
The target must remain connected and powered throughout the operation.

## Storage and verification

The app does not require built-in SD hardware. Attach and mount an SD card at
runtime through a supported expansion, or connect a FAT USB drive in host mode.
Without mounted removable storage, `flash` remains available and asks you to
mount an SD card or USB drive. Internal flash and RAM disks are not used as a
fallback.

SolarOS selects an SD volume first, then USB storage. If several SD volumes are
mounted, it uses the first mount path alphabetically. The browser title shows
the selected cache path. A loaded catalog and its artifact operations keep
that path; attaching another volume does not redirect them. Reopen the app or
refresh the catalog to select storage again.

All data is stored in `.flash` on the selected volume. For a USB drive:

```text
/usb0/.flash/catalog.json
/usb0/.flash/catalog.sig
/usb0/.flash/BOARD/FLAVOR/VERSION/flash-manifest.json
/usb0/.flash/BOARD/FLAVOR/VERSION/factory.bin
```

An SD card uses the same layout under its mount point, usually `/sdcard`.
Keep the cache volume connected throughout an operation. Active USB cache
operations prevent unmounting or switching out of host mode; removal makes
the operation fail. A factory image is verified and programmed through the
same open file, so replacing a drive does not substitute a different image
after verification. With USB storage and a USB serial target, use a powered
hub where needed and respect the shared USB host channel limit.

The catalog signature uses the SolarOS OTA public key. Downloads are streamed
to a staging file, checked against the catalog size and SHA-256, extracted, and
checked again against `flash-manifest.json` before activation. A failed update
keeps the previous cached artifact.

Before erasing the target, SolarOS verifies the cached factory image, opens the
requested serial port exclusively, identifies the target chip, and compares it with
the manifest. It rejects an unsupported or different chip, an undersized flash
device, secure-download mode, flash encryption, or secure boot. A successful
write ends with the target bootloader's MD5 verification.

The board ID is explicit because the ESP ROM reports the chip family, not the
physical board or its pin/display/storage layout. A matching chip is therefore
necessary but is not enough to choose a board artifact safely.

Close Flash when finished to release its working memory. The programming
operation owns the selected serial port and optional control pins until it completes.

## Quick reference

Run `flash refresh` to cache the signed catalog, `flash list` to show artifacts,
and `flash download BOARD FLAVOR [VERSION]` to store one verified factory image
under the selected volume's `.flash` directory. Program it with `flash BOARD FLAVOR` plus optional
`version=`, `port=`, `boot=`, `reset=`, and `baud=` values. The target chip must
match the selected board artifact. Connect crossed 3.3 V UART signals and a
common ground; use both optional control pins for automatic download mode.
