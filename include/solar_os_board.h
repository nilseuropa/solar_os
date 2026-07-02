#pragma once

#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
#elif defined(SOLAR_OS_BOARD_ESP32_S3_DEVKITC1_N16R8)
#include "boards/esp32_s3_devkitc1_n16r8.h"
#elif defined(SOLAR_OS_BOARD_ODROID_GO)
#include "boards/odroid_go.h"
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

#ifndef SOLAR_OS_BOARD_EXPANSION_I2C_BUSES
#define SOLAR_OS_BOARD_EXPANSION_I2C_BUSES {{0}}
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_SPI_BUSES
#define SOLAR_OS_BOARD_EXPANSION_SPI_BUSES {{0}}
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_UART_PORTS
#define SOLAR_OS_BOARD_EXPANSION_UART_PORTS {{0}}
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_ADC_MASK
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK 0ULL
#endif

#ifndef SOLAR_OS_BOARD_EXPANSION_PWM_MASK
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK 0ULL
#endif
