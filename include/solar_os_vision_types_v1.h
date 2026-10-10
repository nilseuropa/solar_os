#pragma once
/* Version 1 native data contract. Layouts and enum values are frozen for this ABI.
 * This header has no ESP-IDF or interpreter dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_image_types_v1.h"

#define SOLAR_OS_VISION_QR_MAX 8U
#define SOLAR_OS_VISION_MAX_WIDTH 640U
#define SOLAR_OS_VISION_MAX_HEIGHT 480U
#define SOLAR_OS_VISION_TIMEOUT_MS 5000U

typedef bool (*solar_os_vision_cancel_fn)(void *user);
typedef struct {
    int32_t x, y;
} solar_os_vision_point_t;
typedef struct {
    solar_os_vision_point_t corners[4]; /* Coordinates in the original image. */
    uint8_t *payload; /* Owned bytes; not necessarily UTF-8 or NUL terminated. */
    size_t length;
    uint32_t eci;
    int version, ecc_level, data_type;
} solar_os_vision_qr_code_t;
typedef struct {
    size_t count;
    uint32_t width, height, processed_width, processed_height;
    uint32_t candidates, decode_failures;
    bool truncated;
    uint64_t preprocess_us, detect_us, decode_us, elapsed_us;
    solar_os_vision_qr_code_t codes[SOLAR_OS_VISION_QR_MAX];
} solar_os_vision_qr_results_t;
