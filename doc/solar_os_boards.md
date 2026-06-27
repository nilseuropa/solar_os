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

## Board Profile

`boards/<target>.cmake` is consumed by `src/CMakeLists.txt`. It gives the board a
stable ID, display name, preprocessor define, and compile-time capability set.

Minimal headless example:

```cmake
set(SOLAR_OS_BOARD_ID "esp32_s3_devkitc1_n16r8")
set(SOLAR_OS_BOARD_NAME "Espressif ESP32-S3-DevKitC-1-N16R8")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8")

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
set(SOLAR_OS_BOARD_DISPLAY_DRIVER "st7305")
set(SOLAR_OS_BOARD_STORAGE_DRIVER "sdmmc")
set(SOLAR_OS_BOARD_RTC_DRIVER "pcf85063")
set(SOLAR_OS_BOARD_BATTERY_DRIVER "adc")
set(SOLAR_OS_BOARD_AUDIO_DRIVER "es8311_es7210")
set(SOLAR_OS_BOARD_SENSOR_DRIVER "shtc3")

set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
set(SOLAR_OS_BOARD_HAS_RTC ON)
set(SOLAR_OS_BOARD_HAS_BATTERY ON)
set(SOLAR_OS_BOARD_HAS_AUDIO ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
set(SOLAR_OS_BOARD_HAS_GPIO ON)
set(SOLAR_OS_BOARD_HAS_ADC ON)
set(SOLAR_OS_BOARD_HAS_PWM ON)
set(SOLAR_OS_BOARD_HAS_KEY ON)
set(SOLAR_OS_BOARD_HAS_TEMPERATURE ON)
set(SOLAR_OS_BOARD_HAS_HUMIDITY ON)
```

Only enable a capability when the board header provides the required pin macros
and the hardware has been checked. Unsupported services still compile, but their
runtime calls return `ESP_ERR_NOT_SUPPORTED`.

Capabilities describe what services should exist. Driver selections describe how
this board implements those capabilities. For example, a board with
`SOLAR_OS_BOARD_HAS_DISPLAY ON` must set `SOLAR_OS_BOARD_DISPLAY_DRIVER`.

Current built-in driver selector values:

| Selector | Values |
| --- | --- |
| `SOLAR_OS_BOARD_DISPLAY_DRIVER` | `st7305` |
| `SOLAR_OS_BOARD_STORAGE_DRIVER` | `sdmmc` |
| `SOLAR_OS_BOARD_RTC_DRIVER` | `pcf85063` |
| `SOLAR_OS_BOARD_BATTERY_DRIVER` | `adc` |
| `SOLAR_OS_BOARD_AUDIO_DRIVER` | `es8311_es7210` |
| `SOLAR_OS_BOARD_SENSOR_DRIVER` | `shtc3` |

## Capability Flags

The current capability flags are:

| Flag | Meaning |
| --- | --- |
| `DISPLAY` | Physical display driver is available. |
| `GFX` | Foreground graphics service can draw to a display. Usually paired with `DISPLAY`. |
| `CDC` | USB serial/JTAG CDC byte-stream port `cdc0`. |
| `UART` | Hardware UART byte-stream port `uart0`. |
| `SD` | SD/MMC storage and filesystem mounting. |
| `I2C` | Board I2C bus is available. |
| `RTC` | RTC attached to the board I2C bus. |
| `BATTERY` | Battery voltage monitor is available. |
| `AUDIO` | Microphone/speaker audio codec path is available. |
| `WIFI` | Wi-Fi station/AP services. |
| `BLE` | BLE keyboard and BLE/GATT services. |
| `GPIO` | Runtime-safe GPIO service. |
| `ADC` | Runtime-safe ADC service. |
| `PWM` | Runtime-safe PWM service. |
| `KEY` | Built-in board key for sleep/pairing control. |
| `TEMPERATURE` | Temperature sensor service. |
| `HUMIDITY` | Humidity sensor service. |

`src/CMakeLists.txt` uses these flags and driver selectors to include board
bindings and low-level driver sources only when they are needed. For example,
`SOLAR_OS_BOARD_HAS_SD` plus `SOLAR_OS_BOARD_STORAGE_DRIVER "sdmmc"` adds
`board/solar_os_board_storage_sdmmc.c` and `drivers/sd_card.c`, while
`SOLAR_OS_BOARD_HAS_UART` adds `drivers/uart_port.c`.

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

## Board Selector

Add the board define to `include/solar_os_board.h`:

```c
#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
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
pio run -e my_board -t upload
pio device monitor -b 115200
```

## Headless Boards

A headless board is a valid SolarOS target as long as it has a byte-stream port.
For boards without `DISPLAY`, SolarOS starts the primary shell on `uart0` when
`UART` is enabled. If `UART` is not available, it falls back to `cdc0` when `CDC`
is enabled.

Recommended minimal capability set for a generic ESP32-S3 board:

```cmake
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
```

With `uart0` as the primary shell, `cdc0` remains clean for logs, a later shell
job, bridge jobs, or host-side tooling.

## Display Boards

For a display board, enable both `DISPLAY` and `GFX` and provide the controller
pin macros expected by the selected driver. The board profile selects the
concrete driver:

```cmake
set(SOLAR_OS_BOARD_DISPLAY_DRIVER "st7305")
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
```

The board header then provides metadata and pins. The current built-in display
driver is for the ST7305 reflective LCD:

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

Different display controllers should get a separate driver and board display
binding instead of overloading the ST7305 macros.

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

Enable these capabilities only when the board profile selects the matching
driver and the board header defines the required bus and pin metadata:

```cmake
set(SOLAR_OS_BOARD_STORAGE_DRIVER "sdmmc")
set(SOLAR_OS_BOARD_RTC_DRIVER "pcf85063")
set(SOLAR_OS_BOARD_BATTERY_DRIVER "adc")
set(SOLAR_OS_BOARD_AUDIO_DRIVER "es8311_es7210")
set(SOLAR_OS_BOARD_SENSOR_DRIVER "shtc3")
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

Audio boards also need I2S and codec power/pin metadata. See the Waveshare board
header for the complete ES8311/ES7210 example.

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
