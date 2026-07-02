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
| `waveshare_esp32_s3_rlcd_4_2` | `waveshare_esp32_s3_rlcd_4_2` | Waveshare ESP32-S3-RLCD-4.2 | Primary ST7305 reflective display target with SDMMC, CDC, UART, RTC, SHTC3, battery ADC, ES8311/ES7210 audio, expansion I2C/UART/GPIO/ADC/PWM, and runtime GPIO1/GPIO2/GPIO3/GPIO17. |
| `odroid_go` | `odroid_go` | Hardkernel ODROID-GO | Classic ESP32 target with ILI9341 display, SD over VSPI/SDSPI, battery ADC, ESP32 DAC speaker, buttons, ADC D-pad, status LED, display brightness, expansion SPI/GPIO/PWM, and runtime GPIO4/GPIO15. |
| `esp32_s3_devkitc1_n16r8` | `esp32_s3_devkitc1_n16r8` | Espressif ESP32-S3-DevKitC-1-N16R8 | Headless ESP32-S3 target with CDC, UART, Wi-Fi, BLE, expansion I2C/SPI/GPIO/ADC/PWM, and no display or onboard sensors. |

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
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_pcf85063.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/sensors_shtc3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_es8311_es7210.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/gpio_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/adc_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/battery_adc.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/pwm_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
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

Only enable a capability when the hardware has been checked and, for pin-backed
peripherals, the board header provides the required pin macros. Unsupported
services still compile, but their runtime calls return `ESP_ERR_NOT_SUPPORTED`.

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
| `DISPLAY` | Physical display driver is available. |
| `GFX` | Foreground graphics service can draw to a display. Usually paired with `DISPLAY`. |
| `CDC` | USB serial/JTAG CDC byte-stream port `cdc0`. |
| `UART` | Hardware UART byte-stream port `uart0`. |
| `SD` | SD/MMC storage and filesystem mounting. |
| `I2C` | Board I2C bus is available. |
| `SPI` | Board SPI bus is available. |
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
| `EXPANSION_I2C` | Expansion connector exposes a usable I2C bus. |
| `EXPANSION_SPI` | Expansion connector exposes a usable SPI bus and chip-select slots. |
| `EXPANSION_UART` | Expansion connector exposes a UART port usable by external hardware. |
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
peripheral. For example, an SPI radio package should require `expansion_spi`
and `expansion_gpio`, not plain `spi` and `gpio`, so it does not build for a
board where SPI exists only for a display or storage device.

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
    {.pin = 1, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 2, .runtime_allowed = true, .role = "expansion"}, \
}
```

Keep the user GPIO list conservative. Do not expose boot strapping pins,
flash/PSRAM pins, display pins, SD pins, I2C pins, or key inputs as runtime GPIO
unless the board design makes that safe.

Expansion bus example:

```c
#include "solar_os_expansion_types.h"

#define SOLAR_OS_BOARD_EXPANSION_I2C_BUSES { \
    {.name = "i2c0", .port = I2C_NUM_0, .sda_pin = GPIO_NUM_8, .scl_pin = GPIO_NUM_9}, \
}
#define SOLAR_OS_BOARD_EXPANSION_SPI_BUSES { \
    { \
        .name = "spi0", \
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
}
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_1) | \
                                           (1ULL << GPIO_NUM_2))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
```

The expansion descriptors describe connector resources available for future
runtime expansion management. They do not claim the resources or initialize
external hardware by themselves.

## Board Selector

Add the board define to `include/solar_os_board.h`:

```c
#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
#elif defined(SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8)
#include "boards/esp32_s3_devkitc1_n16r8.h"
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

## Headless Boards

A headless board is a valid SolarOS target as long as it has a byte-stream port.
For boards without `DISPLAY`, SolarOS starts the primary shell on `uart0` when
`UART` is enabled. If `UART` is not available, it falls back to `cdc0` when `CDC`
is enabled.

Recommended minimal capability set for a generic ESP32-S3 board:

```cmake
set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
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

For the N16R8 module, GPIO35, GPIO36, and GPIO37 are reserved by Octal PSRAM and
must not be exposed as runtime GPIO. The generic DevKitC target also reserves
GPIO38 and GPIO48 because the onboard RGB LED moved between board revisions.
Use a revision-specific board profile if one of those pins must be exposed.

## Display Boards

For a display board, enable both `DISPLAY` and `GFX`, include the display
fragment, and provide the controller pin macros expected by the selected driver:

```cmake
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_st7305.cmake")
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
```

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

Different display controllers should get a separate driver and board display
binding instead of overloading the ST7305 or ILI9341 macros.

The runtime path is:

```text
main.c
  -> solar_os_board_display_*
    -> board/solar_os_board_display_<driver>.c
      -> drivers/<concrete_display_driver>.c
```

`main.c`, terminal, and graphics services should not include concrete display
driver headers.

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
