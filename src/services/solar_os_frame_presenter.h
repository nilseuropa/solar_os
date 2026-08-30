#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_gfx.h"

typedef struct solar_os_frame_presenter solar_os_frame_presenter_t;

typedef struct {
    solar_os_gfx_t *gfx;
    solar_os_display_format_t format;
    uint16_t width;
    uint16_t height;
    uint16_t stride;
    const uint16_t *palette_rgb565;
    size_t palette_size;
    uint16_t preferred_fps;
    bool allow_mono_fallback;
    bool request_high_refresh;
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
esp_err_t solar_os_frame_presenter_deinit(solar_os_frame_presenter_t *presenter);
bool solar_os_frame_presenter_submit(solar_os_frame_presenter_t *presenter,
                                     const uint8_t *data,
                                     size_t data_size);
void solar_os_frame_presenter_take_stats(
    solar_os_frame_presenter_t *presenter,
    solar_os_frame_presenter_stats_t *stats);
