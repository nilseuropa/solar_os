# SolarOS

SolarOS is a small ESP32 operating environment for pocket terminals,
reflective displays, serial consoles, and low-power embedded tools. It provides
a shell, foreground applications, background jobs, storage, networking,
hardware services, Python, and Lua.

## User manual

The [SolarOS User Manual](doc/manual/README.md) is the canonical source for:

- the documentation browsed on GitHub;
- the signed on-device `help` tree and `man`;
- the native agent's SolarOS reference;
- the generated documentation on solar-os.eu.

It contains the complete command, application, job, board, expansion, Python,
Lua, package, and workflow documentation. Edit the topic in `doc/manual/`;
do not maintain a separate device or website copy.

## Build

SolarOS uses PlatformIO with ESP-IDF through the pioarduino Espressif32
platform:

```sh
pio run -e solar_term
pio run -e freenove_esp32_s3_display_4_0
pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
pio run -e odroid_go
pio run -e ttgo_vga32_v14
pio run -e esp32_s3_devkitc1_n16r8
pio run -t upload
pio device monitor -b 115200
```

The default build uses the full firmware flavor, except the 4 MB VGA32 target,
which defaults to `rover`. For a smaller image or an explicit override:

```sh
SOLAR_OS_FLAVOR=core pio run -e solar_term
SOLAR_OS_FLAVOR=writerdeck pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
SOLAR_OS_VGA_MODE=320x200 pio run -e ttgo_vga32_v14
SOLAR_OS_VGA_MODE=320x240 pio run -e ttgo_vga32_v14
```

For an interactive board, update-layout, group, build, and flash workflow, run:

```sh
python3 scripts/os_builder.py
```

The VGA32 target supports build-time `640x480` (default), `640x400`, `320x240`,
and `320x200` VGA modes through `SOLAR_OS_VGA_MODE`.

See [Boards and hardware targets](doc/manual/boards.md) and
[Firmware packages and flavors](doc/manual/packages.md) for the complete build
and target reference.

PlatformIO builds from one checkout are serialized on POSIX hosts to protect
shared ESP-IDF component state. Windows prints a warning because POSIX file
locking is unavailable; do not run concurrent builds from the same checkout.

## Developer references

- [Stream, control, parameter, and OSC binding model](doc/binding-model.md)
- [Service concurrency contract](doc/service-concurrency.md)
- [Memory and task-admission policy](doc/memory-policy.md)
- [OTA release schema](doc/solar_os_ota_schema.md)
- [Manual generation and signed refresh](doc/manual-system.md)

## Contributing

SolarOS follows the principle **"Everything you need. Nothing you don't."**
Changes to the upstream firmware must elevate the supported hardware and provide
clear value to SolarOS users in general. A feature existing in a personal fork
does not by itself make that feature a candidate for the upstream firmware.

- **Applications:** User and community applications should normally be written
  in Python or Lua and distributed through the
  [SolarOS Playground](https://github.com/nilseuropa/solar_os_playground). The
  scripting bindings exist so applications can use native SolarOS services
  without becoming part of the firmware. A native application is accepted only
  at the maintainer's discretion when it benefits the wider SolarOS community
  and complies with SolarOS architecture, resource, package, interface,
  documentation, and testing policies.
- **Board support:** Support for additional boards is welcome. The contributor
  is responsible for validating and maintaining support for boards outside the
  maintainer's primary hardware targets. Pull requests must include build
  results and hardware-in-the-loop test evidence from the actual board; a
  successful compile alone is not hardware validation.
- **Expansion drivers:** Support for additional expansion hardware is welcome
  under the same conditions. The driver must use the SolarOS driver, service,
  resource, capability, and package boundaries, and the contributor is
  responsible for hardware-in-the-loop testing and ongoing regression
  validation on the actual device.

Keep each pull request focused on one feature and base it on the current
upstream branch. Acceptance is decided case by case; contributors should discuss
large native applications, new boards, and expansion drivers before investing
in a substantial implementation.

## Architecture

![Architecture diagram](doc/solar_os_architecture.png)

```text
src/apps/       foreground applications
src/jobs/       background job implementations
src/services/   shared OS services and runtime policy
src/shell/      shell command implementations
src/drivers/    low-level hardware drivers
boards/         TOML board profiles and expansion-driver catalog
scripts/        board-profile generation and desktop configuration tools
packages/       package and flavor catalog
doc/manual/     canonical user manual for GitHub, device, agent, and website
doc/            developer contracts and documentation-system design
```

## Third-party software

SolarOS is licensed under the [Apache License 2.0](LICENSE.md). It also
includes the third-party software below under each project's own license.
Copyright, attribution, patent, and license notices supplied with these
components remain applicable and must be preserved in redistributions.

| Component | Used for | License and attribution |
| --- | --- | --- |
| [Lua 5.4.8](https://www.lua.org/ftp/lua-5.4.8.tar.gz) | Embedded Lua VM and selected standard libraries | MIT; copyright Lua.org, PUC-Rio. The upstream notice is retained in [`lua.h`](components/lua/lua/src/lua.h). |
| [MicroPython `d901e98349`](https://github.com/micropython/micropython/commit/d901e98349) | Embedded Python runtime | MIT; Damien P. George and MicroPython contributors. Notices are retained in the vendored source files. |
| [ESP-DSP 1.8.x](https://github.com/espressif/esp-dsp) | ESP32-S3 PIE-accelerated DSP kernels | Apache-2.0; Espressif Systems and contributors. The managed component includes the upstream `LICENSE` and notice metadata. |
| [minimp3 `ca7c706`](https://github.com/lieff/minimp3/commit/ca7c706001331a5a8e3182ce3b3ce3b243589154) | MP3 decoding | CC0-1.0. The pinned header history credits lieff, Jörn Heusipp, Alibek Omarov, Chris Robinson, Darryl T. Agostinelli, David Reid, Martin Fiedler, and Matthijs van Duin. |
| [stb_image 2.30 (`013ac3b`)](https://github.com/nothings/stb/commit/013ac3beddff3dbffafd5177e7972067cd2b5083) | PNG, JPEG, GIF, and other image decoding | MIT or public domain/Unlicense. The detailed upstream contributor and feature credits are retained in [`stb_image.h`](components/stb_image/include/stb_image.h). |
| [U8g2 `e4a5822`](https://github.com/olikraus/u8g2/commit/e4a582214cd4489307917e5decc8d3ee9597eb4a) | Monochrome graphics, text rendering, and selected display drivers | BSD-2-Clause; olikraus and contributors. See the retained [`LICENSE`](components/u8g2/LICENSE), including its separate font notices. |
| [libwebp `3757b8a`](https://github.com/webmproject/libwebp/commit/3757b8afeb54e305eaef18502812a9a88b7ed662) | WebP decoding | BSD-3-Clause; Google and contributors. See the retained [`AUTHORS`](components/webp_decoder/libwebp/AUTHORS), [`COPYING`](components/webp_decoder/libwebp/COPYING), and [`PATENTS`](components/webp_decoder/libwebp/PATENTS). |
| [MeshCore `03b6ef4`](https://github.com/meshcore-dev/MeshCore/commit/03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a) | Mesh packet, identity, contact, channel, and chat protocol subset | Separate notices are retained for MeshCore, `rweather/Crypto`, and Ed25519. See the [provenance and notice index](src/vendor/meshcore/README.solaros.md). |
| [Peanut-GB `8e65698`](https://github.com/deltabeard/Peanut-GB/commit/8e656982f08663785794b84823d3e27f856fdb7f) | Game Boy emulation and `minigb_apu` audio | MIT; Mahyar Koshkouei, Alex Baines, and contributors. See the retained [provenance and notices](src/vendor/peanut_gb/README.solaros.md). |

The SolarOS ports and local adaptations of minimp3, stb_image, U8g2, and
libwebp were integrated by nilseuropa.
