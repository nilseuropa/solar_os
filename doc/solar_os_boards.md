# Defining SolarOS Boards

SolarOS separates board support into a small board profile, a C header with board
identity and pin metadata, and a PlatformIO environment that selects the target.
The goal is that services and applications can ask for capabilities instead of
assuming the display terminal hardware exists.

## File Layout

For a new board target named `my_board`, add:

```text
boards/my_board.cmake
include/boards/my_board.h
```

Then update:

```text
include/solar_os_board.h
platformio.ini
```

If PlatformIO does not already provide the board definition, also add:

```text
boards/my_board.json
```

Concrete hardware drivers are selected through reusable CMake fragments:

```text
boards/drivers/<driver>.cmake
```

Add a new fragment when a board needs a new concrete display, storage, RTC,
sensor, battery, audio, or port driver. Do not extend `src/CMakeLists.txt` for
each new driver.

## Built-In Targets

The current tree includes these board targets:

| Target | PlatformIO env | Hardware | Highlights |
| --- | --- | --- | --- |
| `waveshare_esp32_s3_rlcd_4_2` | `waveshare_esp32_s3_rlcd_4_2` | Waveshare ESP32-S3-RLCD-4.2 | Primary ST7305 reflective display target with SDMMC, CDC, UART, RTC, SHTC3, battery ADC, ES8311/ES7210 audio, expansion I2C/SPI/UART/GPIO/ADC/PWM, and runtime-routable SPI3 on GPIO1/GPIO2/GPIO3/GPIO17. |
| `elecrow_crowpanel_esp32_s3_4_2_epaper` | `elecrow_crowpanel_esp32_s3_4_2_epaper` | Elecrow CrowPanel ESP32-S3 4.2-inch E-paper | ESP32-S3-WROOM-1-N8R8 target with a 400x300 SSD1683 e-paper display, microSD over SDSPI, CH340C/UART console, rotary/menu/exit controls, status LED, Wi-Fi, BLE, and expansion I2C/SPI/UART/1-Wire/GPIO/ADC/PWM. |
| `odroid_go` | `odroid_go` | Hardkernel ODROID-GO | Classic ESP32 target with ILI9341 display, SD over VSPI/SDSPI, battery ADC, ESP32 DAC speaker, buttons, ADC D-pad, status LED, display brightness, expansion SPI/UART/GPIO/PWM, and runtime GPIO4/GPIO15. |
| `esp32_s3_devkitc1_n16r8` | `esp32_s3_devkitc1_n16r8` | Espressif ESP32-S3-DevKitC-1-N16R8 | Headless ESP32-S3 target with CDC, UART, Wi-Fi, BLE, expansion I2C/SPI/UART/GPIO/ADC/PWM, graphics through attachable display targets, and no primary display or onboard sensors. |

## Board Profile

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
ESP-IDF component dependencies to `SOLAR_OS_BOARD_REQUIRES`, and sets the
matching selector variable. That keeps concrete source/dependency mapping close
to the driver definition.

Current built-in driver selector values:

| Capability | Fragment | Selector |
| --- | --- | --- |
| `CDC` | `drivers/cdc_usb_serial_jtag.cmake` | `SOLAR_OS_BOARD_CDC_DRIVER=usb_serial_jtag` |
| `UART` | `drivers/uart_esp_idf.cmake` | `SOLAR_OS_BOARD_UART_DRIVER=esp_idf` |
| `DISPLAY` | `drivers/display_st7305.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=st7305` |
| `DISPLAY` | `drivers/display_ssd1683.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=ssd1683` |
| `DISPLAY` | `drivers/display_ili9341.cmake` | `SOLAR_OS_BOARD_DISPLAY_DRIVER=ili9341` |
| `SD` | `drivers/storage_sdmmc.cmake` | `SOLAR_OS_BOARD_STORAGE_DRIVER=sdmmc` |
| `SD` | `drivers/storage_sdspi.cmake` | `SOLAR_OS_BOARD_STORAGE_DRIVER=sdspi` |
| `I2C` | `drivers/i2c_esp_idf.cmake` | `SOLAR_OS_BOARD_I2C_DRIVER=esp_idf` |
| `SPI` | `drivers/spi_esp_idf.cmake` | `SOLAR_OS_BOARD_SPI_DRIVER=esp_idf` |
| `RTC` | `drivers/rtc_pcf85063.cmake` | `SOLAR_OS_BOARD_RTC_DRIVER=pcf85063` |
| `TEMPERATURE`, `HUMIDITY` | `drivers/sensors_shtc3.cmake` | `SOLAR_OS_BOARD_SENSOR_DRIVER=shtc3` |
| `AUDIO` | `drivers/audio_es8311_es7210.cmake` | `SOLAR_OS_BOARD_AUDIO_DRIVER=es8311_es7210` |
| `AUDIO` | `drivers/audio_esp32_dac.cmake` | `SOLAR_OS_BOARD_AUDIO_DRIVER=esp32_dac` |
| `GPIO` | `drivers/gpio_esp_idf.cmake` | `SOLAR_OS_BOARD_GPIO_DRIVER=esp_idf` |
| `ADC` | `drivers/adc_esp_idf.cmake` | `SOLAR_OS_BOARD_ADC_DRIVER=esp_idf` |
| `BATTERY` | `drivers/battery_adc.cmake` | `SOLAR_OS_BOARD_BATTERY_DRIVER=adc` |
| `PWM` | `drivers/pwm_esp_idf.cmake` | `SOLAR_OS_BOARD_PWM_DRIVER=esp_idf` |

## Capability Flags

The current capability flags are:

| Flag | Meaning |
| --- | --- |
| `PSRAM` | External PSRAM is present and configured. `SOLAR_OS_BOARD_PSRAM_BYTES` gives the expected capacity. |
| `SIMD` | CPU vector/SIMD instructions are available for bulk data engines such as image, audio, DSP, or accelerated math paths. |
| `DISPLAY` | A board-integrated primary display driver and boot-time display target are available. Requires `GFX`. |
| `GFX` | The firmware can host drawable display targets, including targets registered later by expansion drivers. It does not imply that a display exists at boot. |
| `CDC` | USB serial/JTAG CDC byte-stream port `cdc0`. |
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
| `GPIO` | Runtime-safe GPIO service. |
| `ADC` | Runtime-safe ADC service. |
| `PWM` | Runtime-safe PWM service. |
| `EXPANSION_GPIO` | Expansion connector has runtime-safe GPIO pins for external hardware. |
| `EXPANSION_I2C` | Expansion hardware may use a static or runtime-created named I2C bus. Requires `I2C`. |
| `EXPANSION_SPI` | Expansion hardware may use a static or runtime-created named SPI bus. Requires `SPI`. |
| `EXPANSION_UART` | Expansion hardware may use a static or runtime-created named UART bus. Requires `UART`. |
| `EXPANSION_ADC` | Expansion connector has ADC-capable runtime pins. |
| `EXPANSION_PWM` | Expansion connector has PWM-capable runtime pins. |
| `KEY` | Built-in board key for sleep/pairing control. |
| `BUTTONS` | Built-in digital buttons are available for keyboard/app input. |
| `JOYSTICK` | Built-in analog joystick axes are available for keyboard/app input. |
| `ADC_DPAD` | Built-in ADC D-pad axes are available for keyboard/app input. |
| `STATUS_LED` | Board status LED output is available. |
| `DISPLAY_BRIGHTNESS` | Display backlight or brightness control is available. |
| `TEMPERATURE` | Temperature sensor service. |
| `HUMIDITY` | Humidity sensor service. |

`src/CMakeLists.txt` validates that every enabled driver-backed capability has a
matching selector, then consumes `SOLAR_OS_BOARD_SRCS` and
`SOLAR_OS_BOARD_REQUIRES`. It does not know which concrete source files belong
to ST7305, SDMMC, PCF85063, or any future driver.

Expansion capabilities are compile-time gates for external hardware packages.
Use them when a package needs connector resources rather than an internal board
peripheral. A driver that can use either a static expansion SPI descriptor or a
runtime-routed bus may accept either `expansion_spi` or `expansion_gpio`; a
driver that also requires independent control pins must still require
`expansion_gpio`. Do not gate these packages on plain `spi` and `gpio`, because
those capabilities can refer only to internal display or storage hardware.
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
expansion capabilities.

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
#elif defined(SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8)
#include "boards/esp32_s3_devkitc1_n16r8.h"
#elif defined(SOLAR_OS_BOARD_ODROID_GO)
#include "boards/odroid_go.h"
#elif defined(SOLAR_OS_BOARD_ELECROW_CROWPANEL_ESP32_S3_4_2_EPAPER)
#include "boards/elecrow_crowpanel_esp32_s3_4_2_epaper.h"
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
- GPIO2: MENU, used as the SolarOS KEY for sleep/wake and BLE pairing.
- GPIO6: rotary counter-clockwise/previous, mapped to Up.
- GPIO4: rotary clockwise/next, mapped to Down.
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
MISO, and GPIO11 MOSI, with chip-select slots on GPIO10, GPIO5, GPIO6, and
GPIO7.
The board also permits runtime routing on the spare SPI3 host. Static `spi0`
remains the usual choice; the runtime host is useful for isolated experiments
on another set of routable expansion pins.
Auxiliary SPI displays can use that expansion SPI bus through expansion
drivers. For example, a PCD8544 84x48 LCD module can attach as `lcd0` with
`expansion attach pcd8544 lcd0 spi=spi0 cs=gpio10 dc=gpio4 reset=gpio5` and
then be exercised with `display test lcd0`.
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
the display fragment, and provide the controller pin macros expected by the
selected driver:

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

The Elecrow CrowPanel target uses the SSD1683 e-paper driver on a dedicated SPI
host:

```c
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "SSD1683"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 400
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 300

#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_47
#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_46
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_45
#define SOLAR_OS_BOARD_PIN_LCD_BUSY GPIO_NUM_48
#define SOLAR_OS_BOARD_PIN_LCD_POWER GPIO_NUM_7
```

Its `refresh=auto` default performs fast updates, skips unchanged frames, and
inserts a full waveform on the first update and after every 19 fast updates to
limit ghosting. `display mode display0 refresh=fast` forces fast updates and
`display mode display0 refresh=full` forces the full waveform.

Different display controllers should get a separate driver and board display
binding instead of overloading the ST7305 or ILI9341 macros.

The runtime path is:

```text
main.c
  -> solar_os_board_display_*
    -> board/solar_os_board_display_<driver>.c
      -> drivers/<concrete_display_driver>.c
  -> solar_os_display target display0
```

`main.c`, terminal, and graphics services should not include concrete display
driver headers.

The board panel is registered with the display service as a target with
`source=board` and `role=primary`. Expansion display drivers should remain
expansion drivers for attach/probe/resource management, then register their own
display targets with `source=expansion` when attached.

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
   sd status
   audio status
   battery status
   ```

7. If the board has no display, confirm the primary shell starts on `uart0` and
   that `cdc0` can still be claimed by a job when needed.
