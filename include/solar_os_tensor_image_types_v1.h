#pragma once
/* Version 1 native data contract. Layouts and enum values are frozen for this ABI.
 * This header has no ESP-IDF or interpreter dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_image_types_v1.h"
#include "solar_os_inference_types_v1.h"

typedef enum { SOLAR_OS_TENSOR_NHWC, SOLAR_OS_TENSOR_NCHW,
    SOLAR_OS_TENSOR_HWC, SOLAR_OS_TENSOR_CHW } solar_os_tensor_image_layout_t;
typedef enum { SOLAR_OS_TENSOR_RGB, SOLAR_OS_TENSOR_BGR,
    SOLAR_OS_TENSOR_GRAY } solar_os_tensor_image_color_t;
typedef enum { SOLAR_OS_TENSOR_STRETCH, SOLAR_OS_TENSOR_LETTERBOX } solar_os_tensor_image_resize_t;
typedef struct {
    solar_os_tensor_image_layout_t layout;
    solar_os_tensor_image_color_t color;
    solar_os_tensor_image_resize_t resize;
    uint32_t x, y, width, height;
    double mean[3], std[3];
    uint8_t pad[3]; /* Values in the selected output color order, before normalization. */
} solar_os_tensor_image_options_t;
typedef struct {
    uint32_t source_width, source_height, input_width, input_height;
    uint32_t crop_x, crop_y, crop_width, crop_height;
    uint32_t resized_width, resized_height, pad_left, pad_top;
    uint32_t channels;
    size_t bytes;
    uint64_t preprocess_us;
} solar_os_tensor_image_transform_t;
