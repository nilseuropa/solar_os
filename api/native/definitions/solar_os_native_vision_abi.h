#pragma once
#include "solar_os_native_service_abi.h"
#include "solar_os_vision_types_v1.h"
#include "solar_os_imlib_types_v1.h"
#define SOLAR_OS_NATIVE_QR_SERVICE "vision.qr"
#define SOLAR_OS_NATIVE_QR_ABI 1U
#define SOLAR_OS_NATIVE_IMLIB_SERVICE "vision.imlib"
#define SOLAR_OS_NATIVE_IMLIB_ABI 1U

/* These calls join all native work before returning, including on cancellation.
 * Results and their image/payload members belong to the host until result_free.
 * Retain a transformation image before freeing its result to keep that image. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*qrcodes)(solar_os_raster_image_t *image,
        const solar_os_raster_image_convert_options_t *options,
        solar_os_native_cancel_fn cancel, void *user, solar_os_vision_qr_results_t **result);
    void (*result_free)(solar_os_vision_qr_results_t *result);
} solar_os_native_qr_api_v1_t;
typedef struct {
    uint32_t abi_version, struct_size;
    void (*defaults)(solar_os_imlib_options_t *options);
    int (*run)(solar_os_raster_image_t *image, solar_os_raster_image_t *reference,
        solar_os_imlib_operation_t operation, const solar_os_imlib_options_t *options,
        solar_os_native_cancel_fn cancel, void *user, solar_os_imlib_result_t **result);
    void (*result_free)(solar_os_imlib_result_t *result);
} solar_os_native_imlib_api_v1_t;
