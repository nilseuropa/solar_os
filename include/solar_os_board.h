#pragma once

#if defined(SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_RLCD_4_2)
#include "boards/waveshare_esp32_s3_rlcd_4_2.h"
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
