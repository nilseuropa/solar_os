#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "solar_os_raster_image.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "solar_os_vision_types_v1.h"

/* One admitted native worker at a time; a competing request returns busy
 * (ESP_ERR_INVALID_STATE). No camera ownership or interpreter calls occur.
 * The caller waits cooperatively and may cancel; cancelled/timed-out work is
 * reaped before returning. The worker holds an image reference throughout.
 * Options select a crop/resize; format is ignored (QR uses grayscale).
 * NULL options selects the complete image. Successful results belong to the
 * caller, including an empty result when no code is found. */
esp_err_t solar_os_vision_qrcodes(solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    solar_os_vision_cancel_fn cancel, void *user,
    solar_os_vision_qr_results_t **results);
void solar_os_vision_qr_results_free(solar_os_vision_qr_results_t *results);

#ifdef __cplusplus
}
#endif
