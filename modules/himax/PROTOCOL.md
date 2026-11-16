# Model programmer protocol reference

The implementation follows Seeed's Watcher SPI programmer and SSCMA client at
commit `8e37f7ccd3d836bb99f483e0f6e74e8252406644`:

- [SPI programmer](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/blob/8e37f7ccd3d836bb99f483e0f6e74e8252406644/components/sscma_client/src/sscma_client_flasher_we2_spi.c)
- [SSCMA operations](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/blob/8e37f7ccd3d836bb99f483e0f6e74e8252406644/components/sscma_client/src/sscma_client_ops.c)
- [Factory OTA application](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware/blob/8e37f7ccd3d836bb99f483e0f6e74e8252406644/examples/factory_firmware/main/app/app_ota.c)

The factory application uses the SPI writer for dated firmware beginning
`2024.06.03` and programs model slot 4 at offset `0xA00000`. The writer uses
256-byte pages, XIP base `0x38000000`, ISP enable register `0xD8`, burst enable
`0x13=0x31`, burst mode `0x0D=0x11`, and control word `0x028C208B` at
`0x51010000`. Page completion is read from `0x51010040`; status nibbles 1 and 3
indicate CRC failure. Register transfers use commands `0xF2` and `0xF3`.

SolarOS additionally requires the actual four-slot address inventory, bounds
every destination to its 2 MiB slot, reserves slot 1, and refuses the active slot.
It never implements the vendor's offset-zero firmware/boot-magic write branch.
The module requires explicit replacement authorization and records unknown
existing slot contents honestly. No erase command or address comes from a bundle.

Programming starts with the vendor's hardware reset sequence and verifies the
saved working selection again before entering OTA. Every page has a bounded
completion wait and cancellation checks. The transport
releases the shared SPI bus after each transfer; the camera owner persists
through validation, programming, test inference and selection restoration.
Maintenance DMA is allocated on demand and freed on reset/stop/invalidation.
Periodic worker sleeps preserve scheduling even when status completes immediately.

Page/CRC status and a successful inference are the device-side checks. There is
no full flash SHA-256 readback or transactional rollback. A failed or interrupted
upload can leave its inactive destination partly written. Receipts are written
atomically on storage and never used as authority for future programming.

`INFO?` is queried without rewriting its global model metadata. Bundles retain
their own checked labels and provenance, and receipts bind a programmed slot to
the chip ID and model hash.
