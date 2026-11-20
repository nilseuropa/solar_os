# C64 ROM set

These original Commodore ROM images are embedded in the external C64 module.
They retain their original Commodore copyright, separately from the emulator
code's GPL-3.0-or-later license.

Source: [Zimmers C64 firmware archive](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/c64/).

| Module file | Archive file | Bytes |
| --- | --- | --- |
| `basic.rom` | `basic.901226-01.bin` | 8192 |
| `kernal.rom` | `kernal.901227-03.bin` | 8192 |
| `chargen.rom` | `characters.901225-01.bin` | 4096 |

`sources.json` records each exact source URL, SHA-256 and MD5 digest.
The build verifies the expected sizes and SHA-256 values before generating
the module's read-only arrays. The BASIC and KERNAL images also match the
two halves of the archive's combined C64C `64c.251913-01.bin` image.

`--roms DIRECTORY` overrides this set with external files of the same names
and sizes. The original diagnostic mode uses its own generated guest code.
