#pragma once
#include "solar_os_media_sample.h"
#include <stdbool.h>

typedef struct {
    const char *owner;
    size_t max_bytes;
    uint32_t timeout_ms;
    bool (*cancel)(void *user);
    void *user;
} solar_os_media_sample_source_options_t;
/* Read one owned encoded image from a resolved file, stream:ID, rtsp://URL, or
 * pipeline:ID. Stream/network sources and leases are closed before returning.
 * A pipeline snapshot shares immutable storage with its matching result. */
esp_err_t solar_os_media_sample_source_read(const char *source,
    const solar_os_media_sample_source_options_t *options, solar_os_media_sample_t **out);
