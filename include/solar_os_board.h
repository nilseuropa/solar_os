#pragma once

#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
#elif defined(SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8)
#include "boards/esp32_s3_devkitc1_n16r8.h"
#elif defined(SOLAR_OS_BOARD_ODROID_GO)
#include "boards/odroid_go.h"
#elif defined(SOLAR_OS_BOARD_ELECROW_CROWPANEL_ESP32_S3_4_2_EPAPER)
#include "boards/elecrow_crowpanel_esp32_s3_4_2_epaper.h"
#else
#error "No SolarOS board target selected. Build through a PlatformIO env with a matching boards/<target>.cmake profile."
#endif

#ifndef SOLAR_OS_BOARD_ID
#error "Selected SolarOS board header did not define SOLAR_OS_BOARD_ID."
#endif

#ifndef SOLAR_OS_BOARD_HAS_GPIO
#define SOLAR_OS_BOARD_HAS_GPIO 0
#endif

#ifndef SOLAR_OS_BOARD_GPIO_SLOTS
#if SOLAR_OS_BOARD_HAS_GPIO
#error "Selected SolarOS board header did not define SOLAR_OS_BOARD_GPIO_SLOTS."
#else
#define SOLAR_OS_BOARD_GPIO_SLOTS {{0}}
#endif
#endif

#ifndef SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_USER_GPIO_MASK 0ULL
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_GPIO_MASK
#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK 0ULL
#endif

#ifndef SOLAR_OS_BOARD_BUSES
#define SOLAR_OS_BOARD_BUSES {{0}}
#endif

#ifndef SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK 0U
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_ADC_MASK
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK 0ULL
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_PWM_MASK
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK 0ULL
#endif
