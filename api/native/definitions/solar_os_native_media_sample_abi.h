#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MEDIA_SAMPLE_SERVICE "media.sample"
#define SOLAR_OS_NATIVE_MEDIA_SAMPLE_ABI 1U
#define SOLAR_OS_MEDIA_SAMPLE_MAX_BYTES (4U * 1024U * 1024U)
#define SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX 256U
#define SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE 0U
#define SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO 1U

typedef struct {
    const char *source;
    uint64_t sequence, timestamp_us;
    uint32_t width, height; /* Zero when absent from source metadata. */
} solar_os_media_sample_metadata_t;

typedef struct {
    uint32_t kind;
    const uint8_t *data;
    size_t length;
    /* Image MIME type or audio format (wav/mp3), NULL for unknown images. */
    const char *encoding;
    solar_os_media_sample_metadata_t metadata;
} solar_os_native_media_sample_view_v1_t;

typedef bool (*solar_os_media_sample_cancel_fn)(void *user);
typedef int (*solar_os_native_media_sample_visit_fn)(const solar_os_native_media_sample_view_v1_t *sample, void *user);

typedef struct {
    const char *owner;
    size_t max_bytes;
    uint32_t timeout_ms;
    solar_os_media_sample_cancel_fn cancel;
    void *user;
} solar_os_media_sample_source_options_t;

/* A handle owns one reference to immutable encoded bytes and metadata. A
 * visitor borrows these for the duration of visit. Duplicate a handle to share
 * the same sample with another consumer. Close every handle after use. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*copy)(uint32_t kind, const void *data, size_t length,
        const solar_os_media_sample_metadata_t *metadata, solar_os_native_handle_t *sample);
    int (*open)(uint32_t kind, const char *resolved_path, size_t max_bytes,
        solar_os_media_sample_cancel_fn cancel, void *user, solar_os_native_handle_t *sample);
    /* Encoded image from a resolved file, stream:ID, rtsp://URL or pipeline:ID.
     * Returns NOT_SUPPORTED when the requested source service is unavailable. */
    int (*read_source)(const char *source, const solar_os_media_sample_source_options_t *options,
        solar_os_native_handle_t *sample);
    int (*visit)(solar_os_native_handle_t sample, solar_os_native_media_sample_visit_fn visit, void *user);
    int (*retain)(solar_os_native_handle_t sample, solar_os_native_handle_t *duplicate);
    int (*close)(solar_os_native_handle_t sample);
} solar_os_native_media_sample_api_v1_t;
