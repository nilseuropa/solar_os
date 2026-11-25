#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_inference_types_v1.h"
#include "solar_os_tensor_image_types_v1.h"
#define SOLAR_OS_NATIVE_INFERENCE_SERVICE "inference"
#define SOLAR_OS_NATIVE_INFERENCE_ABI 1U
#define SOLAR_OS_NATIVE_TENSOR_IMAGE_SERVICE "tensor.image"
#define SOLAR_OS_NATIVE_TENSOR_IMAGE_ABI 1U

/* Client destruction drops cancellation/metadata state and never unloads models.
 * Models are shared OS-owned handles; retain while using across concurrent
 * lifecycle controls. Retained models cannot be unloaded. Inputs are borrowed
 * during run; outputs are host-owned until result_free, independent of the model.
 * info is borrowed until the client's next successful info call or destruction.
 * Paths are resolved storage paths; tensor axes have no implicit image meaning.
 * Call blocking operations outside native job tick callbacks. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*create)(solar_os_native_cancel_fn cancel, void *user, solar_os_inference_t **client);
    void (*destroy)(solar_os_inference_t *client);
    int (*load)(solar_os_inference_t *client, const char *path, uint32_t timeout_ms, uint32_t *model);
    int (*load_bundle)(solar_os_inference_t *client, const char *path, uint32_t timeout_ms, uint32_t *model);
    int (*list)(uint32_t *models, size_t capacity, size_t *count);
    int (*find)(const char *path, uint32_t *model);
    int (*info)(solar_os_inference_t *client, uint32_t model, const solar_os_inference_model_info_t **info);
    int (*run)(solar_os_inference_t *client, uint32_t model,
        const solar_os_inference_input_t *inputs, size_t count, uint32_t timeout_ms,
        solar_os_inference_result_t **result);
    int (*run_bundle)(solar_os_inference_t *client, uint32_t model,
        const solar_os_inference_value_t *inputs, size_t count, uint32_t timeout_ms,
        solar_os_inference_result_t **result);
    int (*reset)(solar_os_inference_t *client, uint32_t model);
    int (*set_mode)(solar_os_inference_t *client, uint32_t model, solar_os_inference_mode_t mode);
    int (*unload)(solar_os_inference_t *client, uint32_t model);
    int (*unload_all)(solar_os_inference_t *client);
    int (*retain)(uint32_t model);
    int (*release)(uint32_t model);
    void (*result_free)(solar_os_inference_result_t *result);
} solar_os_native_inference_api_v1_t;

/* Pixel views/ports are borrowed. Caller owns destination storage. */
typedef struct {
    uint32_t abi_version, struct_size;
    void (*defaults)(solar_os_tensor_image_options_t *options);
    int (*inspect)(const solar_os_raster_image_pixels_t *source,
        const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
        solar_os_tensor_image_transform_t *transform);
    int (*prepare)(const solar_os_raster_image_pixels_t *source,
        const solar_os_inference_tensor_t *port, const solar_os_tensor_image_options_t *options,
        uint8_t *destination, size_t length, solar_os_tensor_image_transform_t *transform,
        solar_os_native_cancel_fn cancel, void *user);
} solar_os_native_tensor_image_api_v1_t;
