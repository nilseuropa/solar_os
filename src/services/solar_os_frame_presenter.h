#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_gfx.h"

typedef struct solar_os_frame_presenter solar_os_frame_presenter_t;

typedef enum {
    SOLAR_OS_FRAME_FIT_DEFAULT = 0,
    SOLAR_OS_FRAME_FIT_HEIGHT,
} solar_os_frame_fit_t;

typedef struct {
    solar_os_gfx_t *gfx;
    solar_os_display_format_t format;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    uint16_t preferred_fps;
    solar_os_frame_fit_t fit;
    /* Opt into ordered dithering of indexed or wire-order RGB565 input when
     * the target accepts MONO1 instead of the source format. */
    bool allow_mono_fallback;
    bool request_high_refresh;
    bool reverse_direct_palette;
    /* Preserve supplied colors on direct targets; mono polarity still applies. */
    bool literal_direct_palette;
    /* Convert supplied colors to mono luma without terminal polarity. */
    bool literal_mono_palette;
    bool clear_background_on_resume;
    uint8_t background_index;
    /* Native RGB565 value for letterboxing direct RGB565 frames. */
    uint16_t background_rgb565;
} solar_os_frame_presenter_config_t;

typedef struct {
    uint64_t present_us;
    uint32_t presented_frames;
    uint32_t replaced_frames;
    esp_err_t last_error;
} solar_os_frame_presenter_stats_t;

esp_err_t solar_os_frame_presenter_init(
    solar_os_frame_presenter_t **presenter,
    const solar_os_frame_presenter_config_t *config);
esp_err_t solar_os_frame_presenter_resume(
    solar_os_frame_presenter_t *presenter);
void solar_os_frame_presenter_suspend(solar_os_frame_presenter_t *presenter);
esp_err_t solar_os_frame_presenter_suspend_checked(solar_os_frame_presenter_t *presenter);
esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *presenter);
bool solar_os_frame_presenter_submit(solar_os_frame_presenter_t *presenter,
                                     const uint8_t *data,
                                     size_t data_size);
/* Acquire a host-owned writing buffer once. Exchange queues it without a
 * pixel copy and returns another writing buffer. The completed buffer remains
 * readable until the next exchange. Buffers survive suspend and are freed by
 * deinit; never free them or mix this path with copying submit. */
esp_err_t solar_os_frame_presenter_acquire(solar_os_frame_presenter_t *presenter,
                                          uint8_t **data, size_t *size);
esp_err_t solar_os_frame_presenter_exchange(solar_os_frame_presenter_t *presenter,
                                           uint8_t **data);
bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *presenter);
void solar_os_frame_presenter_take_stats(
    solar_os_frame_presenter_t *presenter,
    solar_os_frame_presenter_stats_t *stats);
