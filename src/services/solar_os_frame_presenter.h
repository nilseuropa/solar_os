#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_display_lease_abi.h"
#include "solar_os_gfx.h"

typedef struct solar_os_frame_presenter solar_os_frame_presenter_t;

typedef struct {
    uint64_t present_us;
    uint32_t presented_frames;
    uint32_t replaced_frames;
    esp_err_t last_error;
} solar_os_frame_presenter_stats_t;

esp_err_t solar_os_frame_presenter_init(
    solar_os_frame_presenter_t **presenter, solar_os_gfx_t *gfx,
    const solar_os_frame_presenter_options_v1_t *config);
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
/* Return the current writing buffer, including while presentation is
 * suspended. The presenter may accept copying submits again after this. */
esp_err_t solar_os_frame_presenter_release(solar_os_frame_presenter_t *presenter,
                                          const uint8_t *data);
bool solar_os_frame_presenter_redraw(solar_os_frame_presenter_t *presenter);
void solar_os_frame_presenter_take_stats(
    solar_os_frame_presenter_t *presenter,
    solar_os_frame_presenter_stats_t *stats);
