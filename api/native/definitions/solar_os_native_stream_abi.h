#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_STREAM_SERVICE "stream"
#define SOLAR_OS_NATIVE_STREAM_ABI 1U
#define SOLAR_OS_NATIVE_STREAM_SCALAR 0U
#define SOLAR_OS_NATIVE_STREAM_EVENT 1U
#define SOLAR_OS_NATIVE_STREAM_BYTES 2U
#define SOLAR_OS_NATIVE_STREAM_AUDIO 3U
#define SOLAR_OS_NATIVE_STREAM_VIDEO 4U
#define SOLAR_OS_NATIVE_STREAM_SOURCE 0U
#define SOLAR_OS_NATIVE_STREAM_SINK 1U
#define SOLAR_OS_NATIVE_STREAM_DUPLEX 2U
#define SOLAR_OS_NATIVE_STREAM_SHARED 0U
#define SOLAR_OS_NATIVE_STREAM_EXCLUSIVE 1U
#define SOLAR_OS_NATIVE_STREAM_FANOUT 2U
#define SOLAR_OS_NATIVE_STREAM_MIXED 3U
#define SOLAR_OS_NATIVE_STREAM_AUDIO_S16_LE 0U
#define SOLAR_OS_NATIVE_STREAM_VIDEO_JPEG 0U
typedef struct {
    uint32_t audio_format, sample_rate, channels, bits_per_sample, frames_per_block;
    uint32_t video_codec, width, height, jpeg_quality;
} solar_os_native_stream_format_v1_t;
typedef struct {
    char id[48], provider[20], device[16], unit[16], format[24], summary[64], owner[24];
    uint32_t type, direction, sharing, active_handles, overruns, underruns, has_scalar_range;
    uint64_t read_units, written_units;
    float scalar_minimum, scalar_maximum;
    solar_os_native_stream_format_v1_t media;
} solar_os_native_stream_info_v1_t;
typedef struct {
    uint32_t direction, timeout_ms;
    solar_os_native_stream_format_v1_t format;
} solar_os_native_stream_options_v1_t;
typedef struct { uint32_t window_ms, timeout_ms; } solar_os_native_stream_read_options_v1_t;
typedef struct { float value; uint64_t uptime_ms; } solar_os_native_stream_sample_v1_t;
typedef struct { const uint8_t *data; size_t size; uint32_t width, height; uint64_t timestamp_us; } solar_os_native_stream_frame_v1_t;
typedef struct {
    uint64_t time_ms, uptime_ms;
    uint32_t has_data, time_valid;
    char line[256], change_key[96];
} solar_os_native_stream_csv_v1_t;
/* Open options and owner are borrowed only during open. A native handle owns
 * its private registry handle and one optional provider-owned frame. Frame
 * data remains borrowed until frame_release; close returns BUSY while leased.
 * Serialize handle operations. Close failure preserves the handle for retry.
 * An endpoint may require I/O and close on the task that opened it.
 * Read/write counts are byte counts. Media formats are explicit values; the
 * private audio/video option union never crosses the ABI. */
typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_stream_info_v1_t *info);
    int (*find)(const char *id, solar_os_native_stream_info_v1_t *info);
    int (*set_unit)(const char *id, const char *unit);
    int (*open)(const char *id, const char *owner, const solar_os_native_stream_options_v1_t *options, solar_os_native_handle_t *handle);
    int (*close)(solar_os_native_handle_t handle);
    bool (*valid)(solar_os_native_handle_t handle);
    int (*read)(solar_os_native_handle_t handle, void *bytes, size_t capacity, uint32_t timeout_ms, size_t *size);
    int (*write)(solar_os_native_handle_t handle, const void *bytes, size_t size, uint32_t timeout_ms, size_t *written);
    int (*read_scalar)(solar_os_native_handle_t handle, const solar_os_native_stream_read_options_v1_t *options, float *value);
    int (*read_sample)(solar_os_native_handle_t handle, const solar_os_native_stream_read_options_v1_t *options, solar_os_native_stream_sample_v1_t *sample);
    int (*observe_sample)(const char *id, const solar_os_native_stream_read_options_v1_t *options, solar_os_native_stream_sample_v1_t *sample);
    int (*scalar_command)(solar_os_native_handle_t handle, float value, uint64_t valid_until_us, uint32_t timeout_ms);
    int (*csv_header)(const char *id, char *header, size_t capacity);
    int (*read_csv)(solar_os_native_handle_t handle, const solar_os_native_stream_read_options_v1_t *options, solar_os_native_stream_csv_v1_t *record);
    int (*frame_acquire)(solar_os_native_handle_t handle, solar_os_native_stream_frame_v1_t *frame);
    int (*frame_release)(solar_os_native_handle_t handle);
    /* Handle information includes its negotiated direction and media format. */
    int (*handle_info)(solar_os_native_handle_t handle, solar_os_native_stream_info_v1_t *info);
    const char *(*type_name)(uint32_t type);
    const char *(*direction_name)(uint32_t direction);
    const char *(*sharing_name)(uint32_t sharing);
    const char *(*audio_format_name)(uint32_t format);
} solar_os_native_stream_api_v1_t;
