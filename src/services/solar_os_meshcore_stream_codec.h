#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE 32U
#define SOLAR_OS_MESHCORE_STREAM_FRAME_MTU 112U
#define SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE 4U
#define SOLAR_OS_MESHCORE_STREAM_ENVELOPE_MAX \
    (SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + \
     SOLAR_OS_MESHCORE_STREAM_FRAME_MTU)

uint32_t solar_os_meshcore_stream_peer_id(
    const uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE]);
bool solar_os_meshcore_stream_envelope_matches(const uint8_t *data,
                                                size_t len);
esp_err_t solar_os_meshcore_stream_envelope_encode(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t *envelope,
    size_t envelope_capacity,
    size_t *envelope_len);
esp_err_t solar_os_meshcore_stream_envelope_decode(
    const uint8_t *envelope,
    size_t envelope_len,
    const uint8_t **frame,
    size_t *frame_len);
