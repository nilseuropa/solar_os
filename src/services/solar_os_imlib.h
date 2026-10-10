#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_raster_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "solar_os_imlib_types_v1.h"

solar_os_imlib_options_t solar_os_imlib_default_options(void);
const char *solar_os_imlib_operation_name(solar_os_imlib_operation_t operation);
bool solar_os_imlib_is_transform(solar_os_imlib_operation_t operation);
bool solar_os_imlib_option_allowed(solar_os_imlib_operation_t operation, const char *key);
/* Optional reference is required only for difference and must have the same
 * source dimensions. Input images remain unchanged. One request at a time;
 * competing calls return busy. Cancellation/error joins the worker before
 * releasing any image/workspace. No interpreter calls in the backend. */
esp_err_t solar_os_imlib_run(solar_os_raster_image_t *image,
    solar_os_raster_image_t *reference, solar_os_imlib_operation_t operation,
    const solar_os_imlib_options_t *options, solar_os_imlib_cancel_fn cancel,
    void *user, solar_os_imlib_result_t **result);
void solar_os_imlib_result_free(solar_os_imlib_result_t *result);

#ifdef __cplusplus
}
#endif
