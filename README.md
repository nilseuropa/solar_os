# SolarOS

SolarOS is an ESP32 operating environment that provides
a shell, foreground applications, background jobs, storage, networking,
hardware services, and scripting surfaces.

**Homage to a simpler life**

SolarOS tries to bring back the spirit of the golden era of home computing: turn on a computer, arrive at a shell prompt, and start doing something. Explore, customize, write a program, make a tool, share it with someone.

* Modern software accumulates layers of convenience that obscure how it works and what it costs to run. We become accustomed to using - and now generating - software without understanding it.

* SolarOS encourages a more deliberate approach: understand the machine, question the complexity, and take responsibility for the resources your software uses. Scripting makes experimentation approachable; native services provide an efficient foundation. The constraints give us a reason to pay attention.

* The scripting surfaces let users express their ideas, while native services handle demanding work, making efficient execution a shared foundation that users can build on.

**Wastefulness for the sake of convenience is not sustainable.**

It is a continuous exploration of how much useful, understandable, general-purpose computing can be built on deliberately constrained hardware. We already have an abundance of computing power; we can and should make better use of it.  

**SolarOS is an argument made executable.**

SolarOS argues that useful, general-purpose computing can be built on modest hardware - and that efficiency, understanding, and the ability to shape your own tools should guide its design.

* The _Solar_ in the name reflects the goal of building an efficient operating environment that can run on solar power. Energy consumption is a design concern alongside memory, storage, and processing time.

* That commitment also extends to the project’s infrastructure: all our servers _( website, HIL pipeline, local expert systems )_ are powered by 100% renewable energy.


## User manual

**The user manual is automatically generated from human validated code by an expert system.**

The [SolarOS User Manual](doc/manual/README.md) is the canonical source for:

- the documentation browsed on GitHub;
- the signed on-device `help` tree and `man`;
- the native agent's SolarOS reference;
- the generated documentation on **solar-os.eu**.

It contains the complete command, application, job, board, expansion, Python,
Lua, package, and workflow documentation. Edit the topic in `doc/manual/`;
do not maintain a separate device or website copy.

Firmware includes a searchable topic directory and a small setup and recovery
guide. On devices with Wi-Fi, PSRAM, and SD storage, run `help update` to install
the signed full manual for the running version. Its guides and scripting API
references remain available offline while the SD card is mounted. See
[Browsing and refreshing documentation](doc/manual/help.md) for setup.

## Scripted vision and model inference

Python and Lua scripts can use native image processing, and
resident model inference through SolarOS services. On ESP32-S3 boards with
PSRAM, use [Zoo](doc/manual/apps.md#zoo) to browse and install model bundles,
then load them with `model bundle PATH` or `solaros.inference.load_bundle`.
Bundles provide checked input preparation and built-in classification or
PICO detection results; other models can expose raw tensors.

[Native image pipelines](doc/manual/pipelines.md) process stored pictures,
camera streams, or JPEG RTSP feeds in background jobs. Scripts can read the
latest results and implement their own display, logging, or application
behavior. Models and pipelines can outlive the scripts that create them;
stop or destroy pipelines and explicitly unload models when finished.
Local cameras are exclusive, so stop another camera owner before opening one.

See [Computer vision](doc/manual/vision.md),
[Model inference](doc/manual/inference.md), and
[Model bundles and pipelines](doc/manual/model-bundles.md) for Python/Lua
examples, package requirements, and model compatibility.

## Build

SolarOS requires PlatformIO Core 6.2.0 or newer and uses ESP-IDF 5.5.5 through
the pinned pioarduino Espressif32 platform release `55.03.312-1`.
Upgrade Core in the Python environment that provides your `pio` command:

```sh
python -m pip install --upgrade 'platformio>=6.2.0'
pio --version
```

Build a target:

```sh
pio run -e solar_term
pio run -e freenove_esp32_s3_display_4_0
pio run -e qdtech_es3c28p
pio run -e qdtech_es3n28p
pio run -e cl_32
pio run -e t_lora_pager
pio run -e t_deck_plus
pio run -e thinknode_m9
pio run -e waveshare_esp32_s3_sim7670g_4g
pio run -e waveshare_esp32_s3_epaper_3_97
pio run -e xteink_x4_pro
pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
pio run -e elecrow_crowpanel_esp32_s3_5_79_epaper
pio run -e odroid_go
pio run -e freenove_esp32_wrover_v3
pio run -e esp32_devkitc_v4_wrover
pio run -e ttgo_vga32_v14
pio run -e esp32_s3_devkitc1_n16r8
pio run -e goouuu_esp32_s3cam
pio run -t upload
pio device monitor -b 115200
```

The default build uses the full firmware flavor, except the e-paper HMI targets,
which default to `writerdeck`, and the 4 MB VGA32 target, which defaults to
`vga32`. For a smaller image or an explicit override:

```sh
SOLAR_OS_FLAVOR=core pio run -e solar_term
SOLAR_OS_FLAVOR=writerdeck pio run -e elecrow_crowpanel_esp32_s3_4_2_epaper
SOLAR_OS_FLAVOR=writerdeck pio run -e elecrow_crowpanel_esp32_s3_5_79_epaper
SOLAR_OS_FLAVOR=writerdeck pio run -e waveshare_esp32_s3_epaper_3_97
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
upstream **beta** branch. Acceptance is decided case by case; contributors should discuss
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
SolarOS builds on open-source libraries and frameworks, and on the knowledge and work shared by their communities. It brings those foundations together into a system that people can study, adapt, and build on.

SolarOS is licensed under the [Apache License 2.0](LICENSE.md). It also
includes the third-party software below under each project's own license.
Copyright, attribution, patent, and license notices supplied with these
components remain applicable and must be preserved in redistributions.

| Component | Used for | License and attribution |
| --- | --- | --- |
| [Lua 5.4.8](https://www.lua.org/ftp/lua-5.4.8.tar.gz) | Embedded Lua VM and selected standard libraries | MIT; copyright Lua.org, PUC-Rio. The upstream notice is retained in [`lua.h`](components/lua/lua/src/lua.h). |
| [MicroPython `d901e98349`](https://github.com/micropython/micropython/commit/d901e98349) | Embedded Python runtime | MIT; Damien P. George and MicroPython contributors. Notices are retained in the vendored source files. |
| [ESP-DSP 1.8.x](https://github.com/espressif/esp-dsp) | ESP32-S3 PIE-accelerated DSP kernels | Apache-2.0; Espressif Systems and contributors. The managed component includes the upstream `LICENSE` and notice metadata. |
| [ESP-DL 3.3.13](https://github.com/espressif/esp-dl/tree/v3.3.13) | Generic resident model inference and ESP32-S3 SIMD kernels | MIT; Espressif Systems and contributors. The managed component retains upstream notices; the local schema copy retains [LICENSE](components/espdl_schema/LICENSE.esp-dl). Reviewed ownership/allocation fixes are applied by [the pinned overlay](scripts/patch_espdl.py). |
| [FlatBuffers 2.0.8](https://github.com/google/flatbuffers/tree/v2.0.8) | Structural verification of ESP-DL model files | Apache-2.0; Google and contributors. See [provenance](components/espdl_schema/README.solaros.md) and [LICENSE](components/espdl_schema/LICENSE.flatbuffers). |
| [esp32-camera 2.1.7 (`202df95`)](https://github.com/espressif/esp32-camera/commit/202df95d7b1dc72e9303ad78f47b8dc9f339e6a1) | ESP32-S3 DVP/SCCB camera capture and OV2640 sensor support | Apache-2.0; Espressif Systems and contributors. The managed component includes the upstream `LICENSE` and notice metadata. |
| [quirc `927d680`](https://github.com/dlbeer/quirc/commit/927d680904dc95fdff4cd9d022eb374b438ff8f2) | Native QR recognition and decoding | ISC; Daniel Beer and contributors. See [provenance and port details](components/quirc/README.solaros.md) and the retained [LICENSE](components/quirc/LICENSE). |
| [OpenMV imlib subset](components/solaros_imlib/README.solaros.md) | Native image statistics, filtering, morphology, differences, and blobs | MIT; OpenMV, LLC and Espressif Systems. The selected kernels and compatibility headers retain their upstream notices. |
| [PicoTTS `bf1a8df`](https://github.com/DiUS/esp-picotts/commit/bf1a8df9d2be1e088a03a775d439ac66e18438dc) | Offline speech synthesis with [runtime-loaded voices](picotts_voices) | Apache-2.0; DiUS Computing, SVOX AG, and contributors. See the retained [`NOTICE`](components/picotts.NOTICE). |
| [minimp3 `ca7c706`](https://github.com/lieff/minimp3/commit/ca7c706001331a5a8e3182ce3b3ce3b243589154) | MP3 decoding | CC0-1.0. The pinned header history credits lieff, Jörn Heusipp, Alibek Omarov, Chris Robinson, Darryl T. Agostinelli, David Reid, Martin Fiedler, and Matthijs van Duin. |
| [stb_image 2.30 (`013ac3b`)](https://github.com/nothings/stb/commit/013ac3beddff3dbffafd5177e7972067cd2b5083) | PNG, JPEG, GIF, and other image decoding | MIT or public domain/Unlicense. The detailed upstream contributor and feature credits are retained in [`stb_image.h`](components/stb_image/include/stb_image.h). |
| [U8g2 `e4a5822`](https://github.com/olikraus/u8g2/commit/e4a582214cd4489307917e5decc8d3ee9597eb4a) | Monochrome graphics, text rendering, and selected display drivers | BSD-2-Clause; olikraus and contributors. See the retained [`LICENSE`](components/u8g2/LICENSE), including its separate font notices. |
| [libwebp `3757b8a`](https://github.com/webmproject/libwebp/commit/3757b8afeb54e305eaef18502812a9a88b7ed662) | WebP decoding | BSD-3-Clause; Google and contributors. See the retained [`AUTHORS`](components/webp_decoder/libwebp/AUTHORS), [`COPYING`](components/webp_decoder/libwebp/COPYING), and [`PATENTS`](components/webp_decoder/libwebp/PATENTS). |
| [MeshCore `03b6ef4`](https://github.com/meshcore-dev/MeshCore/commit/03b6ef4b0de98fc70b49ef10a6d0d61f8381fb7a) | Mesh packet, identity, contact, channel, and chat protocol subset | Separate notices are retained for MeshCore, `rweather/Crypto`, and Ed25519. See the [provenance and notice index](src/vendor/meshcore/README.solaros.md). |
| [Peanut-GB `8e65698`](https://github.com/deltabeard/Peanut-GB/commit/8e656982f08663785794b84823d3e27f856fdb7f) | Game Boy emulation and `minigb_apu` audio | MIT; Mahyar Koshkouei, Alex Baines, and contributors. See the retained [provenance and notices](src/vendor/peanut_gb/README.solaros.md). |

 _Third-party code is adapted and modified it where needed, and integrated it into a shared system designed for constrained hardware by the authors of SolarOS._
