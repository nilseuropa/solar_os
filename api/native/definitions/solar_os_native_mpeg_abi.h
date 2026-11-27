#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MPEG_SERVICE "media.mpeg"
#define SOLAR_OS_NATIVE_MPEG_ABI 1U

typedef struct {
    uint32_t width, height;
    double fps;
    uint32_t sample_rate;
    bool audio;
} solar_os_mpeg_info_t;

typedef struct {
    const uint8_t *y, *cb, *cr;
    size_t y_bytes, chroma_bytes;
    uint32_t width, height, y_stride, chroma_stride;
    double time;
} solar_os_native_mpeg_frame_v1_t;

typedef struct {
    const float *samples; /* Interleaved stereo, including duplicated mono. */
    uint32_t frames, sample_rate;
    double time;
} solar_os_mpeg_audio_t;

typedef int (*solar_os_native_mpeg_video_fn)(const solar_os_native_mpeg_frame_v1_t *frame, void *user);
typedef int (*solar_os_native_mpeg_audio_fn)(const solar_os_mpeg_audio_t *audio, void *user);

/* MPEG-1 program streams with one video track and optional MP2 audio. Frame
 * and sample data are borrowed through visitors. Visitors and cancel callbacks
 * run synchronously, with the decoder held for the duration of the call. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(const char *resolved_path, solar_os_native_cancel_fn cancel, void *user,
        solar_os_native_handle_t *decoder, char *detail, size_t capacity);
    int (*close)(solar_os_native_handle_t decoder);
    int (*info)(solar_os_native_handle_t decoder, solar_os_mpeg_info_t *info);
    int (*error_detail)(solar_os_native_handle_t decoder, char *detail, size_t capacity);
    int (*seek)(solar_os_native_handle_t decoder, double seconds, double *position,
        solar_os_native_cancel_fn cancel, void *user);
    int (*video)(solar_os_native_handle_t decoder, solar_os_native_mpeg_video_fn visit,
        void *user, solar_os_native_cancel_fn cancel, void *cancel_user, bool *ended);
    int (*audio)(solar_os_native_handle_t decoder, solar_os_native_mpeg_audio_fn visit,
        void *user, solar_os_native_cancel_fn cancel, void *cancel_user, bool *ended);
    /* Scale to wire-order RGB565 or one byte of luma per pixel. Enable SIMD
     * after simd_selftest succeeds. */
    int (*raster)(const solar_os_native_mpeg_frame_v1_t *frame, uint8_t *destination,
        size_t capacity, uint32_t width, uint32_t height, bool color, bool simd);
    bool (*simd_available)(void);
    bool (*simd_selftest)(void);
} solar_os_native_mpeg_api_v1_t;
