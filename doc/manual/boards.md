+++
id = "boards"
title = "Boards and hardware targets"
section = "build"
summary = "Supported boards, capabilities, porting structure, and validation"
aliases = ["board", "targets"]
keywords = "boards targets custom board profile fixed expansion platformio waveshare devkit odroid elecrow freenove wrover ttgo vga32 composite vga capabilities porting validation"
packages_any = []
+++
# Defining SolarOS Boards

New board profiles use one versioned TOML manifest in
`boards/manifests/<target>.toml`. The manifest contains identity, MCU family,
build capabilities, buses, pins, connector metadata, and automatic fixed
expansion devices. CMake generates its private board configuration and C header
from that file. Do not edit generated files in `.pio/build/.../generated`.

`boards/expansion_drivers.toml` is the desktop configuration catalog. It states
which MCU families, packages, board capabilities, and wiring bindings each
expansion driver supports. The same catalog is used by the board compiler and
the configuration TUI.

Older built-in boards can still use `boards/<target>.cmake` plus
`include/boards/<target>.h`. This is a migration compatibility path, not the
format for a new custom board. Runtime capability bits remain derived from the
CMake-generated `SOLAR_OS_BOARD_HAS_*` defines.

`scripts/validate_board_metadata.py` checks every board during CMake
configuration. It verifies board registration and identity, capability registry
coverage and dependencies, pin masks against their named lists and free-pin
slots, static bus protocol gates, and the GPIO/bus tables in
`expansion.reference.md`.
Run it directly after changing board metadata:

```sh
python3 scripts/validate_board_metadata.py
```

## File Layout

The source and generated paths are:

```text
boards/manifests/<target>.toml                 source board profile
boards/expansion_drivers.toml                  selectable driver catalog
scripts/board_config.py                        desktop TUI
scripts/generate_board_profile.py              manifest validator/compiler
.pio/build/<env>/generated/solar_os/            generated CMake and C metadata
```

A custom profile normally extends one of the neutral MCU-family base profiles:

```text
esp32_s3_devkitc1_n16r8    ESP32-S3
esp32_devkitc_v4_wrover    classic ESP32 with PSRAM
```

It does not need a new CMake file, C header, selector branch, or PlatformIO
environment. Use the base profile's PlatformIO environment and select the
custom manifest with `SOLAR_OS_BOARD`.

Reusable CMake fragments in `boards/drivers/` remain the package adapters for
concrete board services. The manifest compiler selects them from driver-catalog
metadata. Driver implementation and resource ownership stay in the packaged
expansion driver, so the profile does not introduce driver-specific `#ifdef`
branches.

## Creating A Custom Board With The TUI

Run the tool from the SolarOS source directory:

```sh
python3 scripts/board_config.py
```

The workflow is MCU-first:

1. Select `ESP32-S3` or `Classic ESP32`.
2. Select a compatible neutral base board.
3. Enter the board identity.
4. Select the permanently wired expansion drivers.
5. Select existing buses or create new buses, then enter addresses, pins, chip
   selects, ports, and optional driver parameters.
6. Confirm the generated profile.

The tool rejects unsupported MCU/driver combinations, duplicate device names,
invalid addresses and parameters, unavailable chip-select pins, and GPIO
conflicts. Pins used by a fixed device or a new bus are removed from the runtime
user-pin surface automatically. The output is one inherited TOML file under
`boards/manifests/`.

For example, `devkitc1_epaper_workbench.toml` extends the S3 DevKitC profile and
declares CardKB, SSD1683, and SDSPI as fixed devices. It also declares the second
SPI bus used by the SD card. Build an inherited profile through its base
environment:

```sh
SOLAR_OS_BOARD=devkitc1_epaper_workbench \
  pio run -e esp32_s3_devkitc1_n16r8
```

You can edit the generated TOML after leaving the TUI. Validate it before a
build:

```sh
python3 scripts/generate_board_profile.py \
  --manifest boards/manifests/devkitc1_epaper_workbench.toml \
  --manifest-dir boards/manifests \
  --drivers boards/expansion_drivers.toml \
  --validate-only
```

Fixed devices attach automatically and report `origin=board`. They cannot be
detached at runtime. Use the established names when the device provides a
primary board service:

| Hardware role | Fixed device name |
| --- | --- |
| Primary display | `display0` |
| Removable storage | `storage0` |
| Audio input/output | `audio0` |
| Primary pointer | `touch0` |
| Battery monitor | `battery0` |
| Real-time clock | `rtc0` |
| Environmental sensor | `environment0` |

The profile declares hardware, not policy. Keep job autostart commands in
`.shell/startup`. Do not keep `expansion attach`, `expansion bus create`, or
`session create` commands there for hardware that is now fixed in the profile.

After flashing, verify the automatic devices and their services:

```text
status
pkg
port list
expansion drivers
expansion devices
```

A successful manifest validation and build do not prove wiring, polarity,
power control, or peripheral behavior. Test the generated profile on its
physical target.

## Legacy Multi-File Board Profiles

The procedure below applies only when migrating or maintaining a built-in board
that still uses the legacy CMake/header representation. Do not use it for a new
custom board; use the TUI and one TOML manifest instead.

Use a separate board profile when a supported ESP32 module has permanently
attached hardware that should behave as part of the board. For example, a
product based on the ESP32-S3-DevKitC-1 with a soldered SHTC3 sensor should not
require `expansion attach` after every boot. Its profile can expose the sensor as
the fixed board device `environment0`.

The complete selection path is:

```text
PlatformIO board -> ESP32 or ESP32-S3 target, flash, and PSRAM configuration
SolarOS profile  -> capabilities, compiled drivers, buses, and fixed devices
SolarOS flavor   -> optional applications, jobs, and services
startup script   -> jobs and optional runtime expansion attachments
```

The PlatformIO board and SolarOS profile are different selections. Start with a
PlatformIO board for the actual MCU family and module. An ESP32-S3 profile can
reuse `esp32-s3-devkitc1-n16r8`; a classic ESP32 product must use a compatible
classic ESP32 PlatformIO board and cannot reuse the S3 environment.

### 1. Copy The Closest Profile

Choose a short, stable target ID such as `my_devkit_controller`, then copy the
closest existing CMake profile and board header:

```sh
cp boards/waveshare_esp32_s3_rlcd_4_2.cmake boards/my_devkit_controller.cmake
cp include/boards/waveshare_esp32_s3_rlcd_4_2.h include/boards/my_devkit_controller.h
```

Change the identity in both files. The file stem and both
`SOLAR_OS_BOARD_ID` values must match:

```cmake
set(SOLAR_OS_BOARD_ID "my_devkit_controller")
set(SOLAR_OS_BOARD_NAME "My DevKit Controller")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_MY_DEVKIT_CONTROLLER")
```

```c
#define SOLAR_OS_BOARD_ID "my_devkit_controller"
#define SOLAR_OS_BOARD_NAME "My DevKit Controller"
#define SOLAR_OS_BOARD_VENDOR "My Company"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"
```

### 2. Select Drivers And Capabilities

Keep the reusable port and bus fragments from the base profile. Include a
fragment for each built-in peripheral and enable the service capabilities that
the board provides. For the example SHTC3, add:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/drivers/sensors_shtc3.cmake")

set(SOLAR_OS_BOARD_HAS_TEMPERATURE ON)
set(SOLAR_OS_BOARD_HAS_HUMIDITY ON)
```

The fragment selects the concrete implementation and requires its packaged
expansion driver. If an existing expansion driver has no board fragment, require
its package explicitly instead:

```cmake
list(APPEND SOLAR_OS_BOARD_REQUIRED_PACKAGES expansion_cardkb)
```

A capability is a service contract, not a substitute for a driver. For
example, a primary display needs `DISPLAY` and `GFX`, a matching display driver
fragment, and a fixed `display0` attachment. Hardware with no existing driver
still needs a driver and package implementation before it can be selected by a
board profile.

### 3. Describe Buses, Pins, And Fixed Devices

The board header describes physical facts. Retain or modify the base profile's
named buses and pin definitions to match the real wiring. The cloned DevKitC-1
header already defines `i2c0` on GPIO8 and GPIO9. Declare the soldered SHTC3 as
a fixed device on that bus:

```c
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 1
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "shtc3", \
        .name = "environment0", \
        .binding_count = 2, \
        .bindings = { \
            { \
                .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, \
                .target = "i2c0", \
            }, \
            { \
                .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, \
                .value = 0x70, \
            }, \
        }, \
    }, \
}
```

Board-default devices are attached automatically, report `origin=board`, and
cannot be detached. Their resource bindings may use fixed pins. Use the
established service names when applicable:

| Hardware role | Fixed device name |
| --- | --- |
| Primary display | `display0` |
| Removable storage | `storage0` |
| Audio input/output | `audio0` |
| Primary pointer | `touch0` |
| Battery monitor | `battery0` |
| Real-time clock | `rtc0` |
| Environmental sensor | `environment0` |

Review all pin metadata after adding built-in hardware:

- `SOLAR_OS_BOARD_GPIO_SLOTS` must contain every static-bus and fixed-device
  GPIO. Mark permanently occupied pins `SOLAR_OS_PIN_POLICY_FIXED`.
- `SOLAR_OS_BOARD_USER_GPIO_MASK` and `SOLAR_OS_BOARD_USER_GPIO_LIST` contain
  only pins that users may claim at runtime. Remove built-in hardware pins.
- `SOLAR_OS_BOARD_EXPANSION_GPIO_MASK` and its list describe the physical GPIO
  surface, including signals that can be visible but fixed.
- ADC and PWM expansion masks must be subsets of the runtime-user pins.
- Connector metadata must describe the final product, not the unmodified
  development board.

Fixed devices use the same drivers and services as runtime devices. Drivers
marked as early attach before display and storage startup; other fixed devices
attach during normal peripheral startup.

### 4. Register The Target

Add the board selector to `include/solar_os_board.h`:

```c
#elif defined(SOLAR_OS_BOARD_MY_DEVKIT_CONTROLLER)
#include "boards/my_devkit_controller.h"
```

Add a matching PlatformIO environment:

```ini
[env:my_devkit_controller]
board = esp32-s3-devkitc1-n16r8
board_build.cmake_extra_args = -DSOLAR_OS_BOARD=my_devkit_controller
```

Reuse the base board's partition and SDK configuration only when its flash,
PSRAM, and peripheral settings match the product. Add
`boards/my_devkit_controller.json` if PlatformIO does not already provide a
suitable hardware definition.

### 5. Keep Job Startup Separate

The profile and flavor determine which job packages are compiled. They do not
currently provide a declarative board-job autostart list. Start required jobs
from the selected `.shell/startup` file, for example:

```text
job start batmon 60
job start ntp-sync once
```

The startup script may also recreate optional runtime attachments. Do not put
permanently wired hardware there; declare it as a fixed device in the board
manifest so that resource ownership and service availability are established
by the board.

### 6. Document And Validate The Board

Add the target to the built-in target table below and add matching GPIO and bus
rows to `expansion.reference.md`. The metadata validator deliberately requires
these registrations:

```sh
python3 scripts/validate_board_metadata.py --root . --board my_devkit_controller
pio run -e my_devkit_controller
```

After flashing, verify the compiled packages, ports, and automatic devices:

```text
status
pkg
port list
expansion drivers
expansion devices
```

`expansion devices` should list the permanently attached hardware as automatic
board devices. Test each service on the physical target; a successful build
does not prove the wiring, address, polarity, power control, or peripheral
behavior.

## Built-In Targets

The current tree includes these board targets:

| Target | PlatformIO env | Hardware | Highlights |
| --- | --- | --- | --- |
| `waveshare_esp32_s3_rlcd_4_2` | `waveshare_esp32_s3_rlcd_4_2` | Waveshare ESP32-S3-RLCD-4.2 | Primary ST7305 reflective display target with SDMMC, CDC, UART, RTC, SHTC3, battery ADC, ES8311/ES7210 audio, expansion I2C/SPI/UART/GPIO/ADC/PWM, and runtime-routable SPI3 on GPIO1/GPIO2/GPIO3/GPIO17. |
| `freenove_esp32_s3_display_4_0` | `freenove_esp32_s3_display_4_0` | Freenove ESP32-S3 Display 4.0-inch (FNK0104S) | Integrated 480x320 ST7796 display, FT6336 capacitive pointer, ES8311 speaker and microphone, four-bit SDMMC, battery ADC, native USB CDC, and UART/I2C/GPIO expansion connectors. |
| `elecrow_crowpanel_esp32_s3_4_2_epaper` | `elecrow_crowpanel_esp32_s3_4_2_epaper` | Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | ESP32-S3-WROOM-1-N8R8 target with a 400x300 SSD1683 e-paper display, microSD over SDSPI, CH340C/UART console, rotary/menu/exit controls, status LED, Wi-Fi, BLE, and expansion I2C/SPI/UART/1-Wire/GPIO/ADC/PWM. |
| `odroid_go` | `odroid_go` | Hardkernel ODROID-GO | Classic ESP32 target with ILI9341 display, SD over VSPI/SDSPI, battery ADC, ESP32 DAC speaker, buttons, ADC D-pad, status LED, display brightness, expansion SPI/UART/GPIO/PWM, and runtime GPIO4/GPIO15. |
| `freenove_esp32_wrover_v3` | `freenove_esp32_wrover_v3` | Freenove ESP32-WROVER v3.0 (FNK0060) | Classic ESP32 target with 8 MB PSRAM, CH340/UART console, one-bit SDMMC, Wi-Fi, BLE, a GPIO0 BOOT/KEY button, and a 384x288 monochrome PAL composite display on GPIO25. |
| `ttgo_vga32_v14` | `ttgo_vga32_v14` | LilyGO TTGO VGA32 v1.4 | ESP32-PICO-D4 desktop target with 8 MB external PSRAM, build-selectable 320x200@70Hz, 320x240@60Hz, 640x400@70Hz, or 640x480@60Hz VGA output through the onboard RGB222 resistor DAC, GPIO25 mono DAC audio, a default-attached PS/2 keyboard, v1.4 microSD wiring over HSPI, USB-UART, Wi-Fi, BLE disabled by default, and two input-only expansion GPIOs. |
| `esp32_s3_devkitc1_n16r8` | `esp32_s3_devkitc1_n16r8` | Espressif ESP32-S3-DevKitC-1-N16R8 | Headless ESP32-S3 target with CDC, UART, Wi-Fi, BLE, a GPIO0 BOOT/KEY button, expansion I2C/SPI/UART/GPIO/ADC/PWM, graphics through attachable display targets, and no primary display or onboard sensors. |
| `esp32_devkitc_v4_wrover` | `esp32_devkitc_v4_wrover` | Espressif ESP32-DevKitC V4 with ESP32-WROVER-E | Headless classic ESP32 target with PSRAM, UART, Wi-Fi, BLE, a GPIO0 BOOT/KEY button, expansion I2C/SPI/UART/GPIO/ADC/PWM/I2S, graphics through attachable display targets, and no built-in peripherals. |
| `devkitc1_epaper_workbench` | `esp32_s3_devkitc1_n16r8` with `SOLAR_OS_BOARD=devkitc1_epaper_workbench` | ESP32-S3 DevKitC-1 E-paper Workbench | Manifest-generated development target with fixed CardKB, 400x300 SSD1683 display, and SDSPI storage attachments. |

All built-in 16 MiB targets use `partitions.csv`: each OTA application slot
is 0x700000 bytes (7 MiB), and the internal FAT filesystem partition is
0x1F0000 bytes (1.94 MiB). PlatformIO rejects a firmware image that exceeds an
OTA slot. Devices with an older installed partition table remain OTA-capable
and retain their existing `/flash` size. A full USB/factory flash is required
to install the new table; that migration reformats `/flash`, so copy needed
files off the device first. NVS stays at the same offset unless the whole chip
is explicitly erased.

## Legacy CMake Board Profile

`boards/<target>.cmake` is consumed by `src/CMakeLists.txt`. It gives the board a
stable ID, display name, preprocessor define, and compile-time capability set.

Minimal headless example:

```cmake
set(SOLAR_OS_BOARD_ID "esp32_s3_devkitc1_n16r8")
set(SOLAR_OS_BOARD_NAME "Espressif ESP32-S3-DevKitC-1-N16R8")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/cdc_usb_serial_jtag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_SIMD ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
```

Full board example:

```cmake
set(SOLAR_OS_BOARD_ID "waveshare_esp32_s3_rlcd_4_2")
set(SOLAR_OS_BOARD_NAME "Waveshare ESP32-S3-RLCD-4.2")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/cdc_usb_serial_jtag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_st7305.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdmmc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/spi_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_pcf85063.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/sensors_shtc3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_es8311_es7210.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/gpio_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/adc_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/battery_adc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/pwm_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_SIMD ON)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
set(SOLAR_OS_BOARD_HAS_SPI ON)
set(SOLAR_OS_BOARD_HAS_RTC ON)
set(SOLAR_OS_BOARD_HAS_BATTERY ON)
set(SOLAR_OS_BOARD_HAS_AUDIO ON)
set(SOLAR_OS_BOARD_HAS_AUDIO_INPUT ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
set(SOLAR_OS_BOARD_HAS_GPIO ON)
set(SOLAR_OS_BOARD_HAS_ADC ON)
set(SOLAR_OS_BOARD_HAS_PWM ON)
set(SOLAR_OS_BOARD_HAS_KEY ON)
set(SOLAR_OS_BOARD_HAS_TEMPERATURE ON)
set(SOLAR_OS_BOARD_HAS_HUMIDITY ON)
```

Classic ESP32 display example:

```cmake
set(SOLAR_OS_BOARD_ID "odroid_go")
set(SOLAR_OS_BOARD_NAME "Hardkernel ODROID-GO")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_ODROID_GO")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_ili9341.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdspi.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/battery_adc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_esp32_dac.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/gpio_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/pwm_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 4194304)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_BATTERY ON)
set(SOLAR_OS_BOARD_HAS_AUDIO ON)
set(SOLAR_OS_BOARD_HAS_SPI ON)
set(SOLAR_OS_BOARD_HAS_GPIO ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
set(SOLAR_OS_BOARD_HAS_KEY ON)
set(SOLAR_OS_BOARD_HAS_BUTTONS ON)
set(SOLAR_OS_BOARD_HAS_ADC_DPAD ON)
set(SOLAR_OS_BOARD_HAS_STATUS_LED ON)
set(SOLAR_OS_BOARD_HAS_PWM ON)
set(SOLAR_OS_BOARD_HAS_DISPLAY_BRIGHTNESS ON)
```

Enable a capability when the target can provide that service, either through
built-in hardware or an explicitly supported expansion path. For pin-backed
peripherals, the board header must provide the corresponding static definitions
or runtime routing policy. Packages requiring absent capabilities are pruned.

Capabilities describe what services should exist. Driver fragments describe how
this board implements those capabilities. For example, a board with
`SOLAR_OS_BOARD_HAS_DISPLAY ON` must include a fragment such as
`drivers/display_st7305.cmake`.

Each fragment appends board-specific sources to `SOLAR_OS_BOARD_SRCS`, appends
ESP-IDF component dependencies or required driver packages, and sets the
matching selector variable. That keeps concrete implementation selection close
to the driver definition.

Current built-in driver selector values:

| Capability | Fragment | Selector |
| --- | --- | --- |
| `CDC` | `drivers/cdc_usb_serial_jtag.cmake` | `SOLAR_OS_BOARD_CDC_DRIVER=usb_serial_jtag` |
| `CDC` (dormant) | `drivers/cdc_tinyusb_composite.cmake` | `SOLAR_OS_BOARD_CDC_DRIVER=tinyusb_composite` |
| `UART` | `drivers/uart_esp_idf.cmake` | `SOLAR_OS_BOARD_UART_DRIVER=esp_idf` |
| `DISPLAY` | `drivers/display_st7305.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=st7305` |
| `DISPLAY` | `drivers/display_ssd1683.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=ssd1683` |
| `DISPLAY` | `drivers/display_ili9341.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=ili9341` |
| `DISPLAY` | `drivers/display_st7796.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=st7796` |
| `DISPLAY` | `drivers/display_cvbs_pal.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=cvbs_pal` |
| `DISPLAY` | `drivers/display_vga32.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=vga32` |
| `SD` | `drivers/storage_sdmmc.cmake` | `SOLAR_OS_BOARD_STORAGE_DRIVER=sdmmc` |
| `SD` | `drivers/storage_sdspi.cmake` | `SOLAR_OS_BOARD_STORAGE_DRIVER=sdspi` |
| `I2C` | `drivers/i2c_esp_idf.cmake` | `SOLAR_OS_BOARD_I2C_DRIVER=esp_idf` |
| `SPI` | `drivers/spi_esp_idf.cmake` | `SOLAR_OS_BOARD_SPI_DRIVER=esp_idf` |
| `RTC` | `drivers/rtc_pcf85063.cmake` | `SOLAR_OS_BOARD_RTC_DRIVER=pcf85063` |
| `TEMPERATURE`, `HUMIDITY` | `drivers/sensors_shtc3.cmake` | `SOLAR_OS_BOARD_SENSOR_DRIVER=shtc3` |
| `AUDIO` | `drivers/audio_es8311_es7210.cmake` | `SOLAR_OS_BOARD_AUDIO_DRIVER=es8311_es7210` |
| `AUDIO` | `drivers/audio_es8311_duplex.cmake` | `SOLAR_OS_BOARD_AUDIO_DRIVER=es8311_duplex` |
| `AUDIO` | `drivers/audio_esp32_dac.cmake` | `SOLAR_OS_BOARD_AUDIO_DRIVER=esp32_dac` |
| `GPIO` | `drivers/gpio_esp_idf.cmake` | `SOLAR_OS_BOARD_GPIO_DRIVER=esp_idf` |
| `ADC` | `drivers/adc_esp_idf.cmake` | `SOLAR_OS_BOARD_ADC_DRIVER=esp_idf` |
| `BATTERY` | `drivers/battery_adc.cmake` | `SOLAR_OS_BOARD_BATTERY_DRIVER=adc` |
| `PWM` | `drivers/pwm_esp_idf.cmake` | `SOLAR_OS_BOARD_PWM_DRIVER=esp_idf` |
| `POINTER` | `drivers/pointer_ft6336.cmake` | `SOLAR_OS_BOARD_POINTER_DRIVER=ft6336` |

## Capability Flags

The current capability flags are:

| Flag | Meaning |
| --- | --- |
| `PSRAM` | External PSRAM is present and configured. `SOLAR_OS_BOARD_PSRAM_BYTES` gives the expected capacity. |
| `SIMD` | CPU vector/SIMD instructions are available for bulk data engines such as image, audio, DSP, or accelerated math paths. |
| `DISPLAY` | A board-integrated primary display driver and boot-time display target are available. Requires `GFX`. |
| `GFX` | The firmware can host drawable display targets, including targets registered later by expansion drivers. It does not imply that a display exists at boot. |
| `CDC` | USB Serial/JTAG CDC byte-stream port `cdc0`. The dormant TinyUSB composite driver can add keyboard, mouse, and gamepad HID reports when explicitly enabled. |
| `UART` | Hardware UART service is supported. Named UART buses may be board-defined or created at runtime. |
| `SD` | SD/MMC storage and filesystem mounting. |
| `I2C` | Hardware I2C service is supported. Named I2C buses may be board-defined or created at runtime. |
| `SPI` | Hardware SPI service is supported. Named SPI buses may be board-defined or created at runtime. |
| `RTC` | RTC attached to the board I2C bus. |
| `BATTERY` | Battery voltage monitor is available. |
| `AUDIO` | Speaker/audio-output path is available. |
| `AUDIO_INPUT` | Microphone/audio-input path is available. Usually paired with `AUDIO` on codec boards. |
| `WIFI` | Wi-Fi station/AP services. |
| `BLE` | BLE keyboard and BLE/GATT services. |
| `PS2_KEYBOARD` | A board-integrated PS/2 keyboard bus is available. Requires `GPIO`. |
| `GPIO` | Runtime-safe GPIO service. |
| `ADC` | Runtime-safe ADC service. |
| `PWM` | Runtime-safe PWM service. |
| `EXPANSION_GPIO` | Expansion connector has runtime-safe GPIO pins for external hardware. |
| `EXPANSION_I2C` | Expansion hardware may use a static or runtime-created named I2C bus. Requires `I2C`. |
| `EXPANSION_SPI` | Expansion hardware may use a static or runtime-created named SPI bus. Requires `SPI`. |
| `EXPANSION_UART` | Expansion hardware may use a static or runtime-created named UART bus. Requires `UART`. |
| `EXPANSION_ADC` | Expansion connector has ADC-capable runtime pins. |
| `EXPANSION_PWM` | Expansion connector has PWM-capable runtime pins. |
| `EXPANSION_I2S` | Board has a spare I2S controller and at least three runtime-safe output GPIOs for an external three-wire I2S device. |
| `KEY` | Built-in board key for sleep/pairing control. |
| `BUTTONS` | Built-in digital buttons are available for keyboard/app input. |
| `JOYSTICK` | Built-in analog joystick axes are available as generic axis input. |
| `ADC_DPAD` | Built-in ADC D-pad axes are available for keyboard/app input. |
| `STATUS_LED` | Board status LED output is available. |
| `DISPLAY_BRIGHTNESS` | Display backlight or brightness control is available. |
| `POINTER` | Board-integrated absolute or relative pointing input. Events carry a source, pointer ID, display target, coordinates, deltas, buttons, and press/move/release action. |
| `TEMPERATURE` | Temperature sensor service. |
| `HUMIDITY` | Humidity sensor service. |

`src/CMakeLists.txt` validates that every enabled driver-backed capability has a
matching selector, then consumes direct board sources and required packages. It
does not know which concrete source files belong to ST7305, SDMMC, PCF85063, or
any future driver.

Expansion capabilities are compile-time gates for external hardware packages.
Use them when a package needs connector resources rather than an internal board
peripheral. A driver that can use either a static expansion SPI descriptor or a
runtime-routed bus may accept either `expansion_spi` or `expansion_gpio`; a
driver that also requires independent control pins must still require
`expansion_gpio`. Do not gate these packages on plain `spi` and `gpio`, because
those capabilities can refer only to internal display or storage hardware.
Use `expansion_i2s` for external I2S packages; it is intentionally stricter
than `expansion_gpio` and prevents unusable drivers from entering a board build.
The user-facing connector tables and attachment workflow live in
[Expansion Ports](expansion.md).

## Board Header

`include/boards/<target>.h` contains C-visible board metadata and pin maps.
Every board needs the identity macros:

```c
#pragma once

#define SOLAR_OS_BOARD_ID "my_board"
#define SOLAR_OS_BOARD_NAME "My SolarOS Board"
#define SOLAR_OS_BOARD_VENDOR "Vendor"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"
```

Add only the hardware macros that match enabled capabilities.

UART example:

```c
#include "driver/gpio.h"
#include "driver/uart.h"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44
```

Key example:

```c
#include "driver/gpio.h"

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_18
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 1
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0
```

Runtime GPIO example:

```c
#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_1) | \
                                            (1ULL << GPIO_NUM_2))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_1) | \
                                       (1ULL << GPIO_NUM_2))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "1 2"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "1 2"
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
}
```

Pin policy is separate from physical connector membership:

- `SOLAR_OS_PIN_POLICY_FREE`: available for direct GPIO and future routed buses.
- `SOLAR_OS_PIN_POLICY_RELEASABLE`: has a default board role but may be routed
  after its current service releases it. The board bus descriptor remains
  registered; releasing the service only stops the hardware and frees its pins.
- `SOLAR_OS_PIN_POLICY_FIXED`: never available to runtime pin routing.

Keep the user GPIO list conservative. Do not mark boot strapping, flash/PSRAM,
display, SD, system I2C, or key pins free. A releasable pin remains unavailable
to direct GPIO until a resource-aware service explicitly takes ownership.

Describe the physical placement of every exposed connector contact separately.
The `io` app and `expansion layout [connector]` render this metadata and combine
GPIO contacts with the live pin policy and claim registry:

```c
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "J1 / J3 pin headers"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; antenna at top, USB connectors at bottom"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 22
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 2
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 44
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "J1", .position = 1, .row = 0, .column = 0, \
     .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "J3", .position = 1, .row = 0, .column = 1, \
     .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
}
```

`row` and `column` are zero-based display coordinates. `position` is the
connector manufacturer's pin number and need not increase in screen order.
Use GPIO, power, ground, control, and NC kinds as appropriate; only GPIO entries
participate in live resource lookup. Keep the view description explicit about
which board side is shown and its orientation. A board without this metadata
still builds, but reports that no physical connector map is available.

Static board bus example:

```c
#include "solar_os_bus_types.h"

#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "i2c0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_I2C, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.i2c = { \
            .port = I2C_NUM_0, \
            .sda_pin = GPIO_NUM_8, \
            .scl_pin = GPIO_NUM_9, \
            .speed_hz = SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ, \
        }, \
    }, \
    { \
        .name = "spi0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.spi = { \
            .host = SPI2_HOST, \
            .sclk_pin = GPIO_NUM_12, \
            .miso_pin = GPIO_NUM_13, \
            .mosi_pin = GPIO_NUM_11, \
            .max_transfer_size = 4096, \
            .cs_count = 2, \
            .cs = { \
                {.name = "gpio10", .pin = GPIO_NUM_10}, \
                {.name = "gpio5", .pin = GPIO_NUM_5}, \
            }, \
        }, \
    }, \
}
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK (1U << SPI3_HOST)
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_1) | \
                                           (1ULL << GPIO_NUM_2))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
```

`SOLAR_OS_BOARD_BUSES` is the canonical static-bus table consumed directly by
the protocol-neutral named bus registry. It includes board buses exposed to OS
services and expansion management, such as the Waveshare `i2c0`. Bus names are
unique across protocols. I2C and SPI buses accept shared logical leases; UART
and 1-Wire bus instances are exclusive.
Attaching an expansion device acquires a lease under the device name and
detaching it releases that lease.

Protocol capabilities describe whether the service can exist; they do not
imply a static bus. A board may therefore enable `UART`, `SPI`, or `I2C` with no
matching entry in `SOLAR_OS_BOARD_BUSES` when it supports only runtime-created
buses. Expansion capability flags authorize that runtime-facing path, while
the runtime controller masks and pin policy constrain the instances that may
be created. Configuration checks reject expansion capabilities without their
base protocol and non-empty SPI/UART runtime masks without matching base and
expansion capabilities. `SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK` similarly names
the controller reserved for attachable I2S hardware and must accompany
`EXPANSION_I2S`.

The registry distinguishes immutable board descriptors from runtime-created
buses. Board buses cannot be unregistered. Every protocol uses the same named
bus attach/detach lifecycle. Runtime descriptors are detachable and removable;
board descriptors are detachable only when all signal pins are marked
releasable, and otherwise remain fixed. Detachment releases the hardware
endpoint and signal pins while preserving the name and configuration. Runtime I2C uses an unregistered hardware
controller plus approved SDA/SCL pins. Runtime 1-Wire uses one approved pin.
Runtime SPI is supported on hosts explicitly allowed by
`SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK`; CS entries are bus-owned GPIO slots,
while their logical chip-select use is claimed per device. Runtime bus signal pins and hardware endpoints are
claimed atomically and released when an idle bus is detached or removed. Runtime UART
controllers are limited by `SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK`. An attached
UART reserves its controller and pins, while its driver starts on first lease
and stops on final release.

## Board Selector

Add the board define to `include/solar_os_board.h`:

```c
#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
#elif defined(SOLAR_OS_BOARD_ODROID_GO)
#include "boards/odroid_go.h"
#elif defined(SOLAR_OS_BOARD_MY_BOARD)
#include "boards/my_board.h"
#else
#error "No SolarOS board target selected. Build through a PlatformIO env with a matching boards/<target>.cmake profile."
#endif
```

The define name must match `SOLAR_OS_BOARD_DEFINE` from the board profile.

## PlatformIO Environment

Add an environment in `platformio.ini`:

```ini
[env:my_board]
board = esp32-s3-devkitc-1
board_build.cmake_extra_args = -DSOLAR_OS_BOARD=my_board
```

`board` is the PlatformIO hardware definition. `SOLAR_OS_BOARD` is the SolarOS
profile name under `boards/<target>.cmake`.

When the PlatformIO environment name and SolarOS board profile name are the same,
the CMake argument is still preferred because it removes ambiguity and makes
alias environments possible.

Examples:

```sh
pio run -e my_board
pio run -e odroid_go
pio run -e my_board -t upload
pio device monitor -b 115200
```

Classic ESP32 boards can use a board-specific SDK defaults file when the common
defaults are not appropriate for the target:

```ini
[env:odroid_go]
board = odroid_esp32
board_build.cmake_extra_args = -DSOLAR_OS_BOARD=odroid_go -DSDKCONFIG_DEFAULTS=sdkconfig.defaults.odroid_go
```

## Freenove ESP32-WROVER v3.0

The `freenove_esp32_wrover_v3` target covers the FNK0060 v3.0 board with an
ESP32-WROVER-E-N4R8 module, 4 MB flash, 8 MB physical PSRAM, a CH340 USB-to-UART
bridge, and the rear microSD slot. It uses `uart0` on GPIO1/GPIO3 as the boot
shell and a fixed `storage0` one-bit SDMMC attachment on GPIO14 clock, GPIO15
command, and GPIO2 data.
The active-low BOOT button on GPIO0 is also the SolarOS KEY. A short press uses
the configured sleep or suspend action; another short press resumes from
suspend. A long press forgets the remembered BLE
keyboard and starts pairing. Do not hold the button during reset or power-up,
because GPIO0 low selects the ESP32 serial download boot mode.

The target deliberately leaves the OV2640 camera unsupported. Remove or
disconnect it before using this target. GPIO25 is reserved for PAL composite
output because it conflicts with the camera's VSYNC signal. The other former
camera signals are available as runtime expansion GPIOs; GPIO34, GPIO35,
GPIO36, and GPIO39 are input-only and also support ADC. GPIO5 is a boot
strapping pin, so external circuitry must not force it to the wrong level while
the ESP32 resets.

The `cvbs_pal` backend produces monochrome PAL through the original ESP32's
DAC1 and I2S0 DMA hardware. Its default SolarOS canvas is 384x288 with PAL
625/50 timing. The U8g2 draw buffer lives in PSRAM, while two scanout buffers
in internal RAM swap only at PAL field boundaries so applications keep the
normal display service and do not tear the active field. The timing and
low-level peripheral setup are adapted from LovyanGFX `Panel_CVBS`; SolarOS
keeps its own one-bit graphics stack instead of linking LovyanGFX's separate
color framebuffer.

Small composite displays that do not handle the full PAL raster well can use a
centered 320x200 safe-area mode. Select it when building; the main and virtual
display dimensions both change to 320x200:

```sh
SOLAR_OS_FLAVOR=rover SOLAR_OS_CVBS_MODE=320x200 \
  pio run -e freenove_esp32_wrover_v3
```

Omit `SOLAR_OS_CVBS_MODE` (or set it to `384x288`) to build the default full
PAL mode. Composite scanout requires the ESP32's full 240 MHz clock, so SolarOS
clamps all power profiles to that board-specific CPU floor on this target.

For the Game Boy-focused build, use the full 384x288 PAL mode:

```sh
SOLAR_OS_FLAVOR=rover-retro pio run -e freenove_esp32_wrover_v3
```

This flavor uses the normal system-service baseline and includes BLE keyboard
input, SD access, UART ports, hardware I/O, Files, basic maintenance apps, Log,
Bridge, Wi-Fi, and the SSH/SCP clients. It keeps the rest of the network stack
and unrelated application groups disabled so they do not compete with PAL
scanout and emulation. For serial diagnostics, run:

```text
job start log uart0 debug
```

It is silent because composite scanout owns I2S0. Do not combine
`rover-retro` with `SOLAR_OS_CVBS_MODE=320x200`; the Game Boy image is 320x288.

For the focused Synth build, use:

```sh
SOLAR_OS_FLAVOR=rover-synth pio run -e freenove_esp32_wrover_v3
```

`rover-synth` keeps BLE and PS/2 keyboard input, MIDI, controls, storage, and
basic file tools. It omits Wi-Fi/networking and unrelated application stacks
to retain classic-ESP32 internal memory for Bluetooth and real-time audio.
Attach an LEDC PWM audio output at runtime because PAL scanout owns I2S0:

```text
expansion attach audio-pwm audio pwm=gpio26
synth
```

Connect GPIO25 to the composite input's center conductor and a board GND to its
shield/ground. Keep both leads short and use a PAL-capable input with its normal
75-ohm termination. The backend continuously owns I2S0 and the APLL while the
display is active, so this board cannot use an I2S0 audio backend at the same
time.

Runtime GPIO is available on GPIO4, GPIO5, GPIO13, GPIO18, GPIO19,
GPIO21-GPIO23, GPIO26, GPIO27, GPIO32-GPIO36, and GPIO39. GPIO34-GPIO36 and
GPIO39 are input-only and support runtime ADC; the other runtime pins support
PWM and can form runtime I2C, SPI, UART, or 1-Wire buses. UART0 remains
registered on the CH340 pins and cannot be detached by the shell using it.

The board uses `partitions_4mb.csv`, with one 0x3D0000-byte factory application
slot and a 0x20000-byte (128 KiB) flash filesystem. The board-specific `rover`,
`rover-python`, `rover-lua`, `rover-synth`, and `rover-retro` flavors do not use
a dual-OTA layout on 4 MB flash. Install firmware through the CH340 serial
connection; this partition layout does not support on-device OTA updates.

## LilyGO TTGO VGA32 v1.4

The `ttgo_vga32_v14` target covers the ESP32-PICO-D4 VGA32 revision 1.4 with
8 MB external PSRAM. It uses the onboard RGB222 resistor DAC for VGA, the
USB-UART bridge on UART0, mono ESP32 DAC audio on GPIO25, a PS/2 keyboard on
GPIO32/GPIO33, and the revision 1.4 microSD wiring on HSPI: MOSI GPIO12, MISO
GPIO2, clock GPIO14, and chip select GPIO13. These SD pins differ from the older
revision 1.2 board.

VGA scanout continuously streams a short line ring through I2S1 DMA. A level-3
IRAM interrupt on CPU1 refills completed line groups. Frame submissions are
coalesced, and a CPU1 presentation worker converts the newest monochrome
snapshot into the scanout buffer at a bounded rate. The SolarOS canvas stays
monochrome, while a lookup table expands the configured foreground and
background colors to the board's two-bit-per-channel VGA output. I2S1 and the
VGA GPIOs remain fixed resources while the display is active.

The onboard audio path takes the ESP32 DAC1 signal from GPIO25 and routes the
same mono output to the 3.5 mm jack and the NS4150 speaker amplifier. SolarOS
uses the shared ESP32-DAC backend at 16 kHz. This is output-only hardware; the
board does not advertise microphone or audio-input support. Test it with
`audio tone 880 500`.

The default mode is 640x480@60 Hz. Select another mode at build time with
`SOLAR_OS_VGA_MODE`:

```sh
pio run -e ttgo_vga32_v14
SOLAR_OS_VGA_MODE=320x200 pio run -e ttgo_vga32_v14
SOLAR_OS_VGA_MODE=320x240 pio run -e ttgo_vga32_v14
SOLAR_OS_VGA_MODE=640x400 pio run -e ttgo_vga32_v14
```

The 320x200 and 320x240 modes use double scan and two internal monochrome
scanout buffers. The 320x240 mode derives its 60 Hz timing from 640x480 VGA.
The 640x400@70 Hz and 640x480@60 Hz modes use the standard 25.175 MHz VGA pixel
clock and one internal monochrome scanout buffer to preserve heap for SolarOS.
Updating a high-resolution frame can therefore produce a brief tear while the
new image is copied. Changing `SOLAR_OS_VGA_MODE` causes PlatformIO to
reconfigure CMake automatically.

The board has 4 MB flash, so its PlatformIO environment defaults to the focused
`rover` flavor instead of `full`:

```sh
pio run -e ttgo_vga32_v14
```

The board profile declares `ps2kbd0` and creates the default `keyboard0`
`ps2-keyboard` expansion attachment before the shell. Its topology and input
state remain visible through the generic commands:

```text
expansion devices
input keyboard
input test keyboard0
```

BLE remains available but defaults to off on this board to preserve internal
heap. Use `setterm ble on` and reboot to enable it. `setterm ble default` clears
the user override and restores the board default.

The onboard PS/2 mouse connector is registered as the fixed `ps2mouse0` bus.
Attach a mouse when one is connected with `expansion attach ps2-mouse mouse0
ps2=ps2mouse0`; it is not a default attachment because the connector can be
empty. The microSD signals are fixed board resources; GPIO34 and GPIO39 are the
only header pins available for runtime GPIO/ADC, and both are input-only.

## ODROID-GO

The built-in `odroid_go` target covers the classic ESP32 Hardkernel ODROID-GO.
It uses an ESP32-WROVER module with 4 MiB PSRAM, the ILI9341 display driver,
SDSPI storage on the VSPI bus, battery ADC, ESP32 DAC speaker output, digital
buttons, ADC D-pad input, status LED, PWM display brightness, Wi-Fi, and BLE.

The board does not have CDC, I2C, RTC, onboard temperature/humidity sensors, or
audio input enabled. It boots into the display shell, and `uart0` on GPIO1/GPIO3
is available as the serial byte-stream port.

ODROID-GO uses the shared VSPI bus for the TFT, SD card, and external chip
selects:

- GPIO18: VSPI SCLK
- GPIO19: VSPI MISO
- GPIO23: VSPI MOSI
- GPIO5: TFT chip select
- GPIO22: SD card chip select
- GPIO4 and GPIO15: external IO and runtime-safe SPI chip-select slots

Runtime GPIO access is intentionally limited to GPIO4 and GPIO15. Other visible
or board-significant pins are reserved: GPIO2 is the status LED, GPIO14 is the
LCD backlight, GPIO25 is speaker amplifier enable, GPIO26 is the DAC sample
output, GPIO34/GPIO35 are the ADC D-pad axes, GPIO36 is battery ADC, GPIO39 is
the board key input, and GPIO32/GPIO33/GPIO13/GPIO27/GPIO0 are built-in
buttons.

GPIO25 is amplifier enable/shutdown wiring, not a second SolarOS DAC channel.
Treat GPIO26 as the only DAC sample output for ODROID-GO audio.

## Freenove ESP32-S3 Display 4.0-inch

The `freenove_esp32_s3_display_4_0` target supports the capacitive-touch
FNK0104S board. SolarOS runs its portrait 320x480 ST7796 panel as a 480x320
landscape primary display and controls the active-high GPIO45 backlight with
PWM. The FT6336 touch controller shares `i2c0` with the ES8311 codec and emits
generic absolute pointer events targeted at `display0`. Native apps opt in with
`SOLAR_OS_APP_FLAG_POINTER_EVENTS`; pointer events include press, move, and
release state rather than exposing FT6336 registers to applications. Foreground
Python and Lua apps receive the same routed events through `solaros.input`.
The board profile creates this as the default `ft6336` expansion attachment
named `touch0`; inspect it with `expansion devices`, `input touch`, and `input
test touch0`. Optional logical-range correction is stored with `input calibrate
touch0 ...`.

The ES8311 is configured as one duplex codec: GPIO8 carries playback data to
the codec, GPIO6 carries microphone data to the ESP32-S3, and GPIO1 controls
the active-low speaker amplifier. The SD slot is the fixed four-bit SDMMC
attachment `storage0`. Battery
voltage is measured on GPIO9 through the board divider. The TP4054 circuit is
an analog charger, not a digitally addressable fuel gauge or battery manager.

Expansion wiring is divided across three connectors. `uart0` uses GPIO43 and
GPIO44. The I2C connector exposes GPIO16 SDA and GPIO15 SCL. The GPIO connector
exposes runtime-safe GPIO2, GPIO3, GPIO14, and GPIO21. Current Freenove FNK0104S
documentation identifies the third GPIO signal as GPIO14; GPIO4 is fixed as
the ES8311 MCLK signal. The BOOT button on GPIO0 becomes the SolarOS KEY after
startup; do not hold it during reset because it selects download mode.

Build the target with:

```sh
pio run -e freenove_esp32_s3_display_4_0
```

## Elecrow CrowPanel ESP32-S3 4.2-inch E-paper

The built-in `elecrow_crowpanel_esp32_s3_4_2_epaper` target covers Elecrow's
V1.0 400x300 monochrome CrowPanel. It uses an ESP32-S3-WROOM-1-N8R8 module,
the SSD1683 e-paper driver, microSD over a dedicated SDSPI bus, five digital
controls, a status LED, Wi-Fi, BLE, and an 8 MB dual-OTA partition layout.

Elecrow has shipped both the original panel and a newer panel identified by a
green circular sticker on the back. The newer revision keeps the same GPIO
pinout but requires a different reset, initialization, refresh, and sleep
sequence. SolarOS detects the revision from its post-reset BUSY behavior and
selects the matching command and waveform path automatically.

The board's USB-C data lines terminate at a CH340C USB-to-UART bridge. SolarOS
therefore uses `uart0` on GPIO43/GPIO44 for the serial console and does not
claim native USB Serial/JTAG CDC. The BAT connector supplies the board but the
published schematic does not provide a battery-voltage ADC path, so the target
does not advertise the `BATTERY` capability.

The display shell defaults to the board's landscape orientation, rotated 90
degrees clockwise from the controller's portrait orientation. The onboard
controls are mapped as follows:

- GPIO1: EXIT, mapped to the foreground app-exit key.
- GPIO2: MENU, used as the SolarOS KEY for sleep/suspend control and BLE pairing.
- GPIO6: rotary counter-clockwise/previous, mapped to Down.
- GPIO4: rotary clockwise/next, mapped to Up.
- GPIO5: rotary press, mapped to Enter.

The 20-pin GPIO header exposes GPIO3, GPIO8, GPIO9, GPIO14-GPIO21, and GPIO38.
GPIO3 is a strapping pin and is listed as a physical connector pin but blocked
from runtime control. Runtime GPIO/PWM is allowed on GPIO8, GPIO9, GPIO14-GPIO21,
and GPIO38. ADC is available on the ADC-capable subset GPIO8, GPIO9, and
GPIO14-GPIO20.

The same runtime-safe pins can be routed to named I2C buses on `i2c0` or
`i2c1`, named UART buses on `uart1` or `uart2`, and named 1-Wire buses. They can
also be routed to a named SPI bus on `spi3` after the SD card is unmounted.
SPI3 is arbitrated as one resource: mounting the SD card while an expansion SPI
bus is attached, or creating an expansion SPI bus while SD is mounted, is
rejected.

The panel and storage wiring remains internal board wiring:

- SSD1683: GPIO12 SCK, GPIO11 MOSI, GPIO47 reset, GPIO46 D/C, GPIO45 chip
  select, GPIO48 BUSY, and GPIO7 display-power enable.
- microSD: GPIO39 SCK, GPIO40 MOSI, GPIO13 MISO, GPIO10 chip select, and GPIO42
  SD-power enable.
- GPIO41: active-high status LED.

The target uses `partitions_8mb.csv`, with two 0x3B0000-byte OTA application
slots and a 0x90000-byte flash filesystem partition.

## Headless Boards

A headless board is a valid SolarOS target as long as it has a byte-stream port.
For boards without `DISPLAY`, SolarOS starts the primary shell on `uart0` when
`UART` is enabled. If `UART` is not available, it falls back to `cdc0` when `CDC`
is enabled.

Recommended minimal capability set for a generic ESP32-S3 board:

```cmake
set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_SIMD ON)
include("${CMAKE_CURRENT_LIST_DIR}/drivers/cdc_usb_serial_jtag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
```

With `uart0` as the primary shell, `cdc0` remains clean for logs, a later shell
job, bridge jobs, or host-side tooling.

The built-in `esp32_s3_devkitc1_n16r8` target keeps this headless shell model
and also enables expansion GPIO, ADC, PWM, I2C, and SPI. The default I2C bus is
GPIO8 SDA and GPIO9 SCL. The default SPI bus is FSPI on GPIO12 SCK, GPIO13
MISO, and GPIO11 MOSI, with chip-select slots on GPIO4, GPIO10, GPIO5, GPIO6,
and GPIO7.
Its active-low BOOT button on GPIO0 is also the SolarOS KEY for the configured
short-press sleep/suspend action, light-sleep wake, and long-press BLE keyboard
replacement. GPIO0 remains reserved from runtime routing. Do not hold the
button during reset or power-up, because that selects download boot mode.
The N16R8 target uses the common 16 MiB `partitions.csv` layout described
above. Its internal volume supports durable agent conversations and normal
file workflows without an SD card.
The board also permits runtime routing on the spare SPI3 host. Static `spi0`
remains the usual choice; the runtime host is useful for isolated experiments
on another set of routable expansion pins.
Auxiliary SPI displays can use that expansion SPI bus through expansion
drivers. For example, a PCD8544 84x48 LCD module can attach as `lcd0` with
`expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5` and
then be exercised with `display test lcd0`.
An RFM95W multimode radio wired with NSS on GPIO4 and reset on GPIO5 attaches
with `expansion attach rfm95 radio0 spi=spi0 cs=gpio4 reset=gpio5`.
Auxiliary I2C displays can use `i2c0` as well. A common 128x64 SSD1306 OLED at
address `0x3c` can attach with
`expansion attach ssd1306 oled0 i2c=i2c0 addr=0x3c`; use `display test oled0`
or `session create shell oled0` after attachment. Modules whose image is shifted
two pixels left use the SH1106 profile instead:
`expansion attach sh1106 oled0 i2c=i2c0 addr=0x3c`.

The Waveshare target has no static SPI bus on its expansion connector, but its
spare SPI3 host may be routed over the four free header pins. A full-duplex bus
using GPIO1/GPIO2/GPIO3 plus GPIO17 as its device-select slot is created with:

```text
expansion bus create spi spi1 host=spi3 sclk=gpio1 mosi=gpio2 miso=gpio3 cs=gpio17
```

The bus remains idle until a device attaches. After detaching all devices,
`expansion bus remove spi1` releases the three data/clock pins and its configured
chip-select pins. The board-defined
I2C bus on GPIO13/GPIO14 is fixed and is never remapped by this operation.

The spare I2C/UART controllers and free pins can instead form runtime I2C,
UART, or named 1-Wire buses:

```text
expansion bus create i2c i2c1 port=i2c1 sda=gpio1 scl=gpio2
expansion bus create onewire onewire0 pin=gpio3
expansion bus create uart uart1 port=uart1 tx=gpio1 rx=gpio2
```

The board-defined `uart0` on GPIO43/GPIO44 is non-removable but detachable.
From a display or other non-`uart0` shell, `expansion bus detach uart0` releases those
pins for a temporary runtime bus. Remove the temporary descriptor and run
`expansion bus attach uart0` to restore the board UART. A UART carrying an active port
owner cannot be detached.

For the N16R8 module, GPIO35, GPIO36, and GPIO37 are reserved by Octal PSRAM and
must not be exposed as runtime GPIO. The generic DevKitC target also reserves
GPIO38 and GPIO48 because the onboard RGB LED moved between board revisions.
Use a revision-specific board profile if one of those pins must be exposed.

## Display Boards

For a board-integrated primary display, enable both `DISPLAY` and `GFX`, include
the display fragment, and declare the packaged driver as an immutable early
`display0` attachment in the board header:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_st7305.cmake")
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
```

A headless board that supports attachable graphical displays enables `GFX`
without enabling `DISPLAY`. Its graphics applications are compiled, but they
require a ready named display target at runtime.

The board header then provides metadata and pins. The built-in Waveshare target
uses the ST7305 reflective LCD driver:

```c
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7305"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 400
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 300

#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_40
#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_41
#define SOLAR_OS_BOARD_PIN_LCD_TE GPIO_NUM_6
```

The built-in ODROID-GO target uses the ILI9341 TFT driver on the board VSPI bus:

```c
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ILI9341"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 320

#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_PIN_LCD_MISO GPIO_NUM_19
#define SOLAR_OS_BOARD_PIN_LCD_BL GPIO_NUM_14
```

The Elecrow CrowPanel target exposes its SSD1683 e-paper controller as a fixed
early `display0` expansion attachment on the board-defined `spi0` bus:

```c
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "ssd1683", \
        .name = "display0", \
        .binding_count = 8, \
        .bindings = { \
            {SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"}, \
            {SOLAR_OS_EXPANSION_BINDING_SPI_CS, .target = "spi0", .value = 45}, \
            {SOLAR_OS_EXPANSION_BINDING_GPIO, "dc", .value = 46}, \
            {SOLAR_OS_EXPANSION_BINDING_GPIO, "reset", .value = 47}, \
            {SOLAR_OS_EXPANSION_BINDING_GPIO, "busy", .value = 48}, \
            {SOLAR_OS_EXPANSION_BINDING_GPIO, "power", .value = 7}, \
            {SOLAR_OS_EXPANSION_BINDING_PARAMETER, "clock", .value = 10000}, \
            {SOLAR_OS_EXPANSION_BINDING_PARAMETER, "panel", .value = 0}, \
        }, \
    }, \
}
```

The fixed bindings select GPIO12 SCK, GPIO11 MOSI, GPIO45 CS, GPIO46 D/C,
GPIO47 reset, GPIO48 BUSY, GPIO7 display power, a 10 MHz SPI clock, and the
automatic Elecrow panel profile. That profile detects the original or
green-sticker revision from BUSY behavior and defaults to rotation 2. Runtime
Waveshare V2 attachments use panel profile 3, rotation 0, and 2 MHz by default.

Its `refresh=auto` default performs fast updates, skips unchanged frames, and
inserts a full waveform on the first update and after every 19 fast updates to
limit ghosting. `display mode display0 refresh=fast` forces fast updates and
`display mode display0 refresh=full` forces the full waveform.

Different display controllers should get a separate driver and board display
binding instead of overloading the ST7305 or ILI9341 macros.

The runtime path is:

```text
main.c
  -> early fixed expansion attachments
    -> services/solar_os_<controller>_display.c
      -> drivers/<concrete_display_driver>.c
      -> generic board-display backend for display0
  -> solar_os_display target display0
```

`main.c`, terminal, and graphics services do not include concrete display
driver headers. A board panel is listed as a fixed expansion device and is
registered with the display service as `display0`, `source=board`, and
`role=primary`. When that controller driver is not already attached, a runtime
attachment uses another name and registers an auxiliary target with
`source=expansion`. Each of these controller drivers supports one attached
instance at a time.

## Storage, I2C, Sensors, RTC, And Audio

Enable these capabilities only when the board profile includes the matching
driver fragment and the board header defines the required bus and pin metadata:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdmmc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdspi.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_pcf85063.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/battery_adc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_es8311_es7210.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_esp32_dac.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/sensors_shtc3.cmake")
```

```c
#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_13
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_14

#define SOLAR_OS_BOARD_PIN_SDMMC_CLK GPIO_NUM_38
#define SOLAR_OS_BOARD_PIN_SDMMC_CMD GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_SDMMC_D0 GPIO_NUM_39

#define SOLAR_OS_BOARD_PIN_BATTERY_ADC GPIO_NUM_4
#define SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO 3.0f
```

SDSPI boards provide the shared SPI bus metadata and an SD-card chip select
instead of SDMMC pins. The ODROID-GO target uses VSPI on GPIO18/GPIO19/GPIO23
and `SOLAR_OS_BOARD_PIN_SD_CARD_CS` on GPIO22.
SDMMC boards use those pin macros only in their fixed `storage0` attachment;
the package driver consumes the resulting bindings and is not included through
board-pin `#ifdef` branches.
Boards with a switched card supply can additionally define
`SOLAR_OS_BOARD_PIN_SD_POWER` and `SOLAR_OS_BOARD_SD_POWER_ACTIVE_LEVEL`; the
shared storage adapter enables that rail before probing or mounting the card.

Audio codec boards also need I2S and codec power/pin metadata. See the
Waveshare board header for the complete ES8311/ES7210 example. ESP32 DAC boards
instead define the DAC sample output and optional amplifier-enable pin; the
ODROID-GO target uses GPIO26 for DAC output and GPIO25 for amplifier enable.

The runtime path follows the same pattern as display:

```text
services/solar_os_<service>.c
  -> solar_os_board_<class>_*
    -> board/solar_os_board_<class>_<driver>.c
      -> drivers/<concrete_driver>.c
```

Services and applications should include the board abstraction headers, not
concrete driver headers such as `sd_card.h`, `rtc_pcf85063.h`,
`audio_codec_board.h`, `shtc3.h`, or `battery_adc.h`.

## Validation Checklist

Before committing a new board target:

1. Build the new environment:

   ```sh
   pio run -e my_board
   ```

2. Rebuild the Waveshare environment to catch shared regressions:

   ```sh
   pio run -e waveshare_esp32_s3_rlcd_4_2
   ```

   For changes touching ESP32 classic support, ILI9341 display, SD-SPI,
   ESP32-DAC audio, buttons, or ADC D-pad input, also build ODROID-GO:

   ```sh
   pio run -e odroid_go
   ```

3. Check the compile log for low-level drivers. A headless board should not
   compile display, SD, audio, battery, sensor, or GPIO drivers unless those
   capabilities were explicitly enabled.

4. Flash and verify boot:

   ```sh
   pio run -e my_board -t upload
   ```

5. On the device, run:

   ```text
   status
   port list
   pkg
   ```

6. Try unsupported hardware commands and confirm they fail cleanly, for example:

   ```text
   disk status
   audio status
   battery status
   ```

7. If the board has no display, confirm the primary shell starts on `uart0` and
   that `cdc0` can still be claimed by a job when needed.

## Quick reference

Select the PlatformIO environment matching the physical target. Board profiles
declare capabilities and drivers; flavors select packages within those
capabilities. The built-in target table, pin rules, display/storage/audio
details, porting procedure, and validation checklist are maintained here.
