#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_media_protocol_abi.h"

typedef void (*solar_os_media_release_fn)(void *token);

typedef struct {
    solar_os_media_track_t track;
    uint64_t frame_id;
    uint64_t timestamp_us;
    uint32_t duration_us;
    const uint8_t *data;
    size_t length;
    bool discontinuity;
    bool end_of_stream;
    solar_os_media_release_fn release;
    void *release_token;
} solar_os_media_unit_t;

bool solar_os_media_track_valid(const solar_os_media_track_t *track);
bool solar_os_media_unit_valid(const solar_os_media_unit_t *unit);
void solar_os_media_unit_release(solar_os_media_unit_t *unit);

esp_err_t solar_os_media_clock_init(solar_os_media_clock_t *clock,
                                    uint64_t origin_us,
                                    uint32_t origin_timestamp,
                                    uint32_t clock_rate);
esp_err_t solar_os_media_clock_map(const solar_os_media_clock_t *clock,
                                   uint64_t timestamp_us,
                                   uint32_t *rtp_timestamp);
