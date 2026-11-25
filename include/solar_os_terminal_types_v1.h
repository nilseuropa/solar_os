#pragma once
#include "solar_os_application_types_v1.h"

/* 792 px at the narrowest 5 px cell fits 156 columns inside the margins. */
#define SOLAR_OS_TERMINAL_MAX_COLS 160
#define SOLAR_OS_TERMINAL_MAX_ROWS 64

typedef enum {
    SOLAR_OS_TERMINAL_FONT_MONO,
    SOLAR_OS_TERMINAL_FONT_COMPACT,
} solar_os_terminal_font_t;

typedef enum {
    SOLAR_OS_TERMINAL_CURSOR_BLOCK,
    SOLAR_OS_TERMINAL_CURSOR_UNDERSCORE,
} solar_os_terminal_cursor_shape_t;

typedef enum {
    SOLAR_OS_TERMINAL_TEXT_SIZE_14 = 0,
    SOLAR_OS_TERMINAL_TEXT_SIZE_20 = 1,
    SOLAR_OS_TERMINAL_TEXT_SIZE_12 = 2,
    SOLAR_OS_TERMINAL_TEXT_SIZE_16 = 3,
    SOLAR_OS_TERMINAL_TEXT_SIZE_18 = 4,
    SOLAR_OS_TERMINAL_TEXT_SIZE_10 = 5,
    SOLAR_OS_TERMINAL_TEXT_SIZE_8 = 6,
    SOLAR_OS_TERMINAL_TEXT_SIZE_COUNT,
} solar_os_terminal_text_size_t;

typedef struct {
    uint16_t orientation_degrees;
    solar_os_terminal_font_t font;
    solar_os_terminal_text_size_t text_size;
    bool palette_inverted;
    bool status_bar_visible;
    uint8_t scale;
} solar_os_terminal_profile_t;

typedef struct {
    uint16_t inbox_unread;
    bool battery_valid;
    uint8_t battery_percent;
    bool battery_external_power;
    uint8_t keyboard_count;
    char keyboard_layout[3];
    bool bluetooth_supported;
    bool bluetooth_enabled;
    bool bluetooth_scanning;
    bool wifi_started;
    bool wifi_connected;
    bool wifi_has_ip;
    uint8_t wifi_level;
    bool audio_enabled;
    uint8_t audio_volume;
    bool time_valid;
    uint8_t hour;
    uint8_t minute;
    bool sd_mounted;
    bool radio_attached;
    bool link_running;
} solar_os_status_bar_t;
