#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_stream.h"
#include "solar_os_synth_types_v1.h"

typedef struct {
    const char *owner;
    /* NULL or empty selects the current default audio output. */
    const char *playback_stream;
    solar_os_synth_render_cb_t render;
    void *user;
    size_t block_frames;
} solar_os_synth_config_t;

/* One client owns the synthesizer and audio output at a time. */
esp_err_t solar_os_synth_start(const solar_os_synth_config_t *config);
esp_err_t solar_os_synth_stop(const char *owner);
void solar_os_synth_get_status(solar_os_synth_status_t *status);
