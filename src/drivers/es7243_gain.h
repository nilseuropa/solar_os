#pragma once
#include <stdbool.h>
#include <math.h>
#include "esp_err.h"

/* Match esp_codec_dev's discrete hardware gain selection and report the
 * selected value, rather than caching an unattainable requested gain. */
static inline esp_err_t solar_os_es7243_gain(bool variant_e, float requested, float *applied)
{
    if (!applied || !isfinite(requested) || requested < 0 || requested > (variant_e ? 37.5f : 27.0f))
        return ESP_ERR_INVALID_ARG;
    if (variant_e) {
        float rounded = requested + 0.5f;
        *applied = rounded <= 33.0f ? (float)((unsigned)rounded / 3U * 3U) :
            rounded < 36.0f ? 34.5f : rounded < 37.0f ? 36.0f : 37.5f;
    } else {
        *applied = requested <= 1 ? 1.0f : requested <= 4 ? 3.5f : requested <= 18 ? 18.0f :
            requested < 21 ? 20.5f : requested < 23 ? 22.5f : requested < 25 ? 24.5f : 27.0f;
    }
    return ESP_OK;
}
