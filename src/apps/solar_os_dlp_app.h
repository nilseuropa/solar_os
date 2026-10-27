#pragma once
#include "solar_os.h"
#include "solar_os_display.h"
#include "solar_os_stream.h"

typedef struct {
    uint32_t frames;
    uint32_t render_last_us, render_max_us;
    uint32_t display_last_us, display_max_us;
    uint32_t total_last_us, total_max_us;
    uint64_t render_total_us, display_total_us, total_us;
    uint64_t first_completed_us, last_completed_us;
} solar_os_dlp_presentation_stats_t;

typedef struct {
    bool running, suspended, waiting;
    char display[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char stream[SOLAR_OS_STREAM_ID_MAX];
    uint32_t received_bytes, sent_bytes, presented_frames;
    solar_os_dlp_presentation_stats_t presentation;
} solar_os_dlp_app_status_t;

extern const solar_os_app_t solar_os_dlp_app;
void solar_os_dlp_app_get_status(solar_os_dlp_app_status_t *status);
