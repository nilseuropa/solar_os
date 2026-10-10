#pragma once
/* Version 1 native data contract. Layouts and enum values are frozen for this ABI.
 * This header has no ESP-IDF or interpreter dependency. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_image_types_v1.h"

#define SOLAR_OS_INFERENCE_MODELS_MAX 4U
#define SOLAR_OS_INFERENCE_PORTS_MAX 16U
#define SOLAR_OS_INFERENCE_RANK_MAX 8U
#define SOLAR_OS_INFERENCE_NAME_MAX 128U
#define SOLAR_OS_INFERENCE_TENSOR_MAX (4U * 1024U * 1024U)
#define SOLAR_OS_INFERENCE_FILE_MAX (16U * 1024U * 1024U)
#define SOLAR_OS_INFERENCE_TIMEOUT_MAX_MS 60000U
#define SOLAR_OS_INFERENCE_BACKEND_VERSION "3.3.13"

typedef struct solar_os_inference solar_os_inference_t;
typedef bool (*solar_os_inference_cancel_fn)(void *user);
typedef enum {
    SOLAR_OS_INFERENCE_SINGLE, SOLAR_OS_INFERENCE_AUTO, SOLAR_OS_INFERENCE_DUAL,
} solar_os_inference_mode_t;
typedef enum {
    SOLAR_OS_TENSOR_INT8, SOLAR_OS_TENSOR_UINT8,
    SOLAR_OS_TENSOR_INT16, SOLAR_OS_TENSOR_UINT16,
    SOLAR_OS_TENSOR_INT32, SOLAR_OS_TENSOR_UINT32,
    SOLAR_OS_TENSOR_INT64, SOLAR_OS_TENSOR_UINT64,
    SOLAR_OS_TENSOR_FLOAT32, SOLAR_OS_TENSOR_FLOAT64,
    SOLAR_OS_TENSOR_FLOAT16, SOLAR_OS_TENSOR_BOOL,
} solar_os_tensor_dtype_t;

/* Dense tensors in the model's native axis order. Shape has no implicit image
 * meaning. Numeric buffers use little-endian elements. Exponents describe
 * ESP-DL power-of-two quantization: real = stored * 2**exponent; zero point 0.
 * A per-channel vector is exposed verbatim; no axis meaning is guessed. */
typedef struct {
    char name[SOLAR_OS_INFERENCE_NAME_MAX];
    solar_os_tensor_dtype_t dtype;
    uint32_t rank, shape[SOLAR_OS_INFERENCE_RANK_MAX];
    size_t bytes;
    size_t exponent_count;
    int32_t *exponents;
} solar_os_inference_tensor_t;

typedef struct {
    size_t input_count, output_count, model_bytes, internal_bytes, external_bytes;
    solar_os_inference_mode_t mode;
    uint32_t references;
    char path[256];
    bool bundle;
    char bundle_id[101], bundle_version[101];
    bool image_inputs[SOLAR_OS_INFERENCE_PORTS_MAX];
    solar_os_inference_tensor_t inputs[SOLAR_OS_INFERENCE_PORTS_MAX];
    solar_os_inference_tensor_t outputs[SOLAR_OS_INFERENCE_PORTS_MAX];
} solar_os_inference_model_info_t;

typedef struct {
    const char *name;
    const void *data;
    size_t bytes;
    /* Optional typed contract; NULL interprets raw bytes using the model port.
     * When present, shape/dtype/bytes and quantization must match exactly. */
    const solar_os_inference_tensor_t *tensor;
} solar_os_inference_input_t;

typedef struct {
    size_t count;
    uint64_t input_us, inference_us, output_us, elapsed_us;
    uint64_t preprocess_us, postprocess_us;
    char *result_json, *transforms_json;
    struct {
        solar_os_inference_tensor_t tensor;
        void *data;
    } outputs[SOLAR_OS_INFERENCE_PORTS_MAX];
} solar_os_inference_result_t;

/* A bundle input borrows either a dense tensor or immutable image pixels. */
typedef struct {
    solar_os_inference_input_t tensor;
    const solar_os_raster_image_pixels_t *image;
} solar_os_inference_value_t;
