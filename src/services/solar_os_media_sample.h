#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES (4U * 1024U * 1024U)
#define SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX 256U

typedef struct solar_os_media_sample solar_os_media_sample_t;
typedef struct {
    const char *source;
    uint64_t sequence, timestamp_us;
    uint32_t width, height; /* Zero when not supplied by the source. */
} solar_os_media_sample_metadata_t;
typedef struct {
    const uint8_t *data;
    size_t length;
    const char *mime_type; /* Detected from bytes; NULL for unknown encodings. */
    solar_os_media_sample_metadata_t metadata;
} solar_os_media_sample_image_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    const char *format; /* Encoded container: "wav" or "mp3". */
    solar_os_media_sample_metadata_t metadata;
} solar_os_media_sample_audio_t;
typedef bool (*solar_os_media_sample_cancel_fn)(void *user);

/* Immutable owned encoded image. Copies both bytes and source metadata without
 * decoding or transcoding. A provider lease may be released as soon as this
 * returns. Unknown encodings remain usable by a consumer's image decoder. */
esp_err_t solar_os_media_sample_copy_image(const void *data, size_t length,
    const solar_os_media_sample_metadata_t *metadata, solar_os_media_sample_t **out);
esp_err_t solar_os_media_sample_open_image(const char *resolved_path,
    solar_os_media_sample_t **out);
esp_err_t solar_os_media_sample_open_image_bounded(const char *resolved_path,
    size_t max_bytes, solar_os_media_sample_t **out);
/* Audio remains encoded and immutable. Container recognition does not decode
 * or transcode it. File reads poll cancellation between bounded chunks. */
esp_err_t solar_os_media_sample_copy_audio(const void *data, size_t length,
    const solar_os_media_sample_metadata_t *metadata, solar_os_media_sample_t **out);
esp_err_t solar_os_media_sample_open_audio_bounded(const char *resolved_path,
    size_t max_bytes, solar_os_media_sample_cancel_fn cancel, void *user,
    solar_os_media_sample_t **out);
const solar_os_media_sample_audio_t *solar_os_media_sample_audio(
    const solar_os_media_sample_t *sample);
/* Borrowed view; retain the sample for the complete use, including uploads. */
const solar_os_media_sample_image_t *solar_os_media_sample_image(
    const solar_os_media_sample_t *sample);
void solar_os_media_sample_retain(solar_os_media_sample_t *sample);
void solar_os_media_sample_release(solar_os_media_sample_t *sample);
