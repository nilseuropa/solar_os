#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "solar_os_display_maintenance.h"

/* Shared recovery transaction. The adapter pauses ordinary presentation and
 * supplies synchronous native full refreshes without changing its mode.
 * The saved image stays separate from black/white scratch frames, including
 * when a failed phase is followed by END to restore the original image.
 */
typedef struct { uint8_t *storage; } epd_relax_t;
typedef esp_err_t (*epd_relax_native_fn)(void *context,
    solar_os_display_relax_phase_t phase, const uint8_t *image);

static inline void epd_relax_dispose(epd_relax_t *state)
{
    heap_caps_free(state->storage);
    state->storage = NULL;
}

static inline esp_err_t epd_relax_apply(epd_relax_t *state, uint8_t *buffer,
    size_t bytes, solar_os_display_relax_phase_t phase,
    epd_relax_native_fn native, void *context)
{
    if (!state || !buffer || !bytes || bytes > SIZE_MAX / 2 || !native ||
        (unsigned)phase > SOLAR_OS_DISPLAY_RELAX_END) return ESP_ERR_INVALID_ARG;
    if (phase == SOLAR_OS_DISPLAY_RELAX_BEGIN) {
        if (state->storage) return ESP_ERR_INVALID_STATE;
        uint8_t *storage = heap_caps_malloc(bytes * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!storage) storage = heap_caps_malloc(bytes * 2, MALLOC_CAP_8BIT);
        if (!storage) return ESP_ERR_NO_MEM;
        memcpy(storage, buffer, bytes);
        state->storage = storage;
        const esp_err_t err = native(context, phase, NULL);
        if (err != ESP_OK) epd_relax_dispose(state);
        return err;
    }
    if (!state->storage) return ESP_ERR_INVALID_STATE;
    const bool ending = phase == SOLAR_OS_DISPLAY_RELAX_END;
    uint8_t *image = state->storage + (ending ? 0 : bytes);
    if (!ending) memset(image, phase == SOLAR_OS_DISPLAY_RELAX_WHITE ? 0xff : 0, bytes);
    const esp_err_t err = native(context, phase, image);
    if (ending) {
        memcpy(buffer, state->storage, bytes);
        epd_relax_dispose(state);
    }
    return err;
}
