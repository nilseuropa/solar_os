#pragma once

#include "solar_os_native_audio_abi.h"
#include "solar_os_native_stream_abi.h"

#define SOLAR_OS_NATIVE_AUDIO_DECODER_SERVICE "media.audio.decoder"
#define SOLAR_OS_NATIVE_AUDIO_DECODER_ABI 1U

typedef struct {
    size_t max_samples;
    size_t stream_window_bytes;
} solar_os_audio_decoder_limits_v1_t;

typedef struct {
    solar_os_native_stream_format_v1_t format;
    size_t consumed_bytes;
    size_t source_frames, decoded_frames, decoded_samples;
    size_t payload_bytes;
} solar_os_audio_decoder_frame_v1_t;

/* Select a concrete encoded format from media.audio. Use limits to discover
 * support and size the interleaved S16 output and the input window. Probe
 * inspects a byte window for audio format without opening a decoder.
 *
 * Process consumes bytes from the beginning of the caller's input window
 * and reports at most one frame. Keep unconsumed bytes for the next call.
 * Supply at least stream_window_bytes before each call on a live stream.
 * At end of input, pass the remaining bytes as a shorter window.
 * Input and PCM output must use separate buffers.
 *
 * Pass NULL and zero for PCM to scan frame headers without synthesis.
 * Reset and decode a warm-up prefix when seeking before audible output.
 * On successful calls, consumed_bytes can be nonzero with no PCM. This
 * includes skipped sync bytes and frames whose reservoir is incomplete.
 * source_frames counts encoded duration and payload_bytes excludes the frame
 * header, checksum and side information when present.
 *
 * Serialize reset and process on each decoder. Close releases its state. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*limits)(uint32_t format, solar_os_audio_decoder_limits_v1_t *limits);
    int (*probe)(uint32_t format, const void *input, size_t input_len,
        solar_os_native_stream_format_v1_t *pcm_format);
    int (*open)(uint32_t format, solar_os_native_handle_t *decoder);
    int (*reset)(solar_os_native_handle_t decoder);
    int (*process)(solar_os_native_handle_t decoder, const void *input, size_t input_len,
        int16_t *pcm, size_t pcm_capacity_samples, solar_os_audio_decoder_frame_v1_t *frame);
    int (*close)(solar_os_native_handle_t decoder);
} solar_os_native_audio_decoder_api_v1_t;
