#pragma once
#include "solar_os_native_audio_routes_abi.h"
const void *solar_os_native_stream_get_service(const char *name, uint32_t abi, uint32_t minimum);
int solar_os_native_stream_open_default_audio(const char *owner,
    const solar_os_native_stream_options_v1_t *options, solar_os_native_handle_t *out);
int solar_os_native_stream_capture_audio(solar_os_native_handle_t id, size_t frames,
    int16_t *samples, size_t sample_capacity);
int solar_os_native_stream_measure_audio(solar_os_native_handle_t id, uint32_t duration_ms,
    int channel, solar_os_audio_level_t *level);
