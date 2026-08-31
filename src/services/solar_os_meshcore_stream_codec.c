#include "solar_os_meshcore_stream_codec.h"

#include <string.h>

#define MESHCORE_STREAM_MAGIC_0 0x53U
#define MESHCORE_STREAM_MAGIC_1 0x4dU
#define MESHCORE_STREAM_ENVELOPE_VERSION 1U

uint32_t solar_os_meshcore_stream_peer_id(
    const uint8_t public_key[SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE])
{
    if (public_key == NULL) {
        return 0U;
    }
    uint32_t hash = 2166136261U;
    for (size_t index = 0U;
         index < SOLAR_OS_MESHCORE_STREAM_PUBLIC_KEY_SIZE;
         index++) {
        hash ^= public_key[index];
        hash *= 16777619U;
    }
    if (hash == 0U || hash == UINT32_MAX) {
        hash ^= 0xa5a5a5a5U;
    }
    return hash;
}

bool solar_os_meshcore_stream_envelope_matches(const uint8_t *data,
                                                size_t len)
{
    return data != NULL &&
           len >= SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE &&
           data[0] == MESHCORE_STREAM_MAGIC_0 &&
           data[1] == MESHCORE_STREAM_MAGIC_1;
}

esp_err_t solar_os_meshcore_stream_envelope_encode(
    const uint8_t *frame,
    size_t frame_len,
    uint8_t *envelope,
    size_t envelope_capacity,
    size_t *envelope_len)
{
    if (envelope_len != NULL) {
        *envelope_len = 0U;
    }
    if (frame == NULL || frame_len == 0U ||
        frame_len > SOLAR_OS_MESHCORE_STREAM_FRAME_MTU ||
        envelope == NULL || envelope_len == NULL ||
        envelope_capacity <
            SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + frame_len) {
        return ESP_ERR_INVALID_ARG;
    }
    envelope[0] = MESHCORE_STREAM_MAGIC_0;
    envelope[1] = MESHCORE_STREAM_MAGIC_1;
    envelope[2] = MESHCORE_STREAM_ENVELOPE_VERSION;
    envelope[3] = (uint8_t)frame_len;
    memcpy(&envelope[SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE],
           frame,
           frame_len);
    *envelope_len =
        SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + frame_len;
    return ESP_OK;
}

esp_err_t solar_os_meshcore_stream_envelope_decode(
    const uint8_t *envelope,
    size_t envelope_len,
    const uint8_t **frame,
    size_t *frame_len)
{
    if (frame != NULL) {
        *frame = NULL;
    }
    if (frame_len != NULL) {
        *frame_len = 0U;
    }
    if (!solar_os_meshcore_stream_envelope_matches(envelope, envelope_len) ||
        frame == NULL || frame_len == NULL ||
        envelope[2] != MESHCORE_STREAM_ENVELOPE_VERSION) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t decoded_len = envelope[3];
    if (decoded_len == 0U ||
        decoded_len > SOLAR_OS_MESHCORE_STREAM_FRAME_MTU ||
        envelope_len <
            SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + decoded_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t index =
             SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE + decoded_len;
         index < envelope_len;
         index++) {
        if (envelope[index] != 0U) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    *frame = &envelope[SOLAR_OS_MESHCORE_STREAM_ENVELOPE_HEADER_SIZE];
    *frame_len = decoded_len;
    return ESP_OK;
}
