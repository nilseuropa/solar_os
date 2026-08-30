#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_gfx.h"
#include "solar_os_terminal.h"
#include "u8g2.h"

typedef struct solar_os_board_display solar_os_board_display_t;

typedef const char *(*solar_os_display_mode_getter_t)(const void *context);
typedef esp_err_t (*solar_os_display_mode_setter_t)(void *context, const char *mode);
typedef esp_err_t (*solar_os_display_surface_presenter_t)(
    void *context,
    const solar_os_display_surface_t *surface);
typedef esp_err_t (*solar_os_display_frame_presenter_t)(
    void *context,
    const solar_os_display_raster_t *frame);

#define SOLAR_OS_DISPLAY_TARGET_MAX 6
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define SOLAR_OS_DISPLAY_TARGET_SOURCE_MAX 12
#define SOLAR_OS_DISPLAY_TARGET_DRIVER_MAX 20
#define SOLAR_OS_DISPLAY_TARGET_CONTROLLER_MAX 20
#define SOLAR_OS_DISPLAY_TARGET_ROLE_MAX 16
#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32
#define SOLAR_OS_DISPLAY_PRIMARY_TARGET "display0"

typedef struct {
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char source[SOLAR_OS_DISPLAY_TARGET_SOURCE_MAX];
    char driver[SOLAR_OS_DISPLAY_TARGET_DRIVER_MAX];
    char controller[SOLAR_OS_DISPLAY_TARGET_CONTROLLER_MAX];
    char role[SOLAR_OS_DISPLAY_TARGET_ROLE_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    uint16_t width;
    uint16_t height;
    bool ready;
    bool brightness_supported;
    bool black_is_one;
    uint32_t surface_formats;
    uint32_t frame_formats;
    uint16_t preferred_stream_fps;
    uint32_t max_stream_pixels_per_second;
    u8g2_t *u8g2;
    const u8g2_cb_t *base_rotation;
    void *controller_context;
    solar_os_display_mode_getter_t controller_mode;
    solar_os_display_mode_getter_t controller_mode_values;
    solar_os_display_mode_setter_t set_controller_mode;
    void *surface_context;
    solar_os_display_surface_presenter_t present_surface;
    void *frame_context;
    solar_os_display_frame_presenter_t present_frame;
} solar_os_display_target_t;

typedef enum {
    SOLAR_OS_DISPLAY_PRESENT_TEXT,
    SOLAR_OS_DISPLAY_PRESENT_GRAPHICS,
} solar_os_display_present_mode_t;

typedef struct {
    const uint8_t *data;
    size_t data_size;
    uint32_t frame_id;
    uint32_t target_generation;
    uint16_t width;
    uint16_t height;
    uint16_t native_width;
    uint16_t native_height;
    uint16_t native_stride;
    uint8_t target_slot;
    solar_os_display_rotation_t rotation;
    bool black_is_one;
} solar_os_display_frame_t;

esp_err_t solar_os_display_init(solar_os_board_display_t *display);
esp_err_t solar_os_display_register_target(const solar_os_display_target_t *target);
esp_err_t solar_os_display_unregister_target(const char *name);
size_t solar_os_display_target_count(void);
bool solar_os_display_get_target(size_t index, solar_os_display_target_t *target);
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target);
bool solar_os_display_target_name_for_u8g2(const u8g2_t *u8g2,
                                           char *name,
                                           size_t name_len);
esp_err_t solar_os_display_get_terminal_profile(
    const char *name,
    solar_os_terminal_profile_t *profile);
esp_err_t solar_os_display_set_terminal_profile(
    const char *name,
    const solar_os_terminal_profile_t *profile);
/* Claims are reference-counted per owner; every successful claim needs a release. */
esp_err_t solar_os_display_claim(const char *name,
                                 const char *owner,
                                 char *busy_owner,
                                 size_t busy_owner_len);
esp_err_t solar_os_display_open_gfx(const char *name,
                                    const char *owner,
                                    solar_os_gfx_t **gfx,
                                    char *busy_owner,
                                    size_t busy_owner_len);
esp_err_t solar_os_display_release(const char *name, const char *owner);
bool solar_os_display_brightness_supported(void);
esp_err_t solar_os_display_get_brightness(uint8_t *percent);
esp_err_t solar_os_display_set_brightness(uint8_t percent);
esp_err_t solar_os_display_get_colors(uint32_t *foreground_rgb888,
                                      uint32_t *background_rgb888);
esp_err_t solar_os_display_set_foreground_color(uint32_t rgb888);
esp_err_t solar_os_display_set_background_color(uint32_t rgb888);
esp_err_t solar_os_display_suspend_primary(void);
esp_err_t solar_os_display_resume_primary(void);
bool solar_os_display_primary_suspended(void);
esp_err_t solar_os_display_set_palette_inverted(const char *name, bool inverted);
esp_err_t solar_os_display_get_controller_mode(const char *name,
                                               const char **mode,
                                               const char **values);
esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode);
/* Non-persistent performance override; disabling restores driver settings. */
esp_err_t solar_os_display_set_high_refresh_override(const char *name,
                                                     bool enabled,
                                                     uint16_t hz_tenths);
esp_err_t solar_os_display_request_present_mode(u8g2_t *u8g2,
                                                solar_os_display_present_mode_t mode);
void solar_os_display_present(u8g2_t *u8g2, solar_os_display_present_mode_t mode);
esp_err_t solar_os_display_present_surface(
    u8g2_t *u8g2,
    const solar_os_display_surface_t *surface);
esp_err_t solar_os_display_present_frame(
    u8g2_t *u8g2,
    const solar_os_display_raster_t *frame);
esp_err_t solar_os_display_present_mono_xbm(u8g2_t *u8g2,
                                            const uint8_t *bitmap,
                                            size_t bitmap_size,
                                            uint16_t x,
                                            uint16_t y,
                                            uint16_t width,
                                            uint16_t height,
                                            uint16_t stride,
                                            bool palette_inverted);

/*
 * Frame export keeps one PSRAM snapshot per exported target. Readers hold an
 * immutable reference while transmitting; presentation skips publication
 * rather than waiting for network I/O.
 */
esp_err_t solar_os_display_start_frame_export(const char *name);
void solar_os_display_stop_frame_export(const char *name);
esp_err_t solar_os_display_acquire_frame(const char *name, solar_os_display_frame_t *frame);
void solar_os_display_release_frame(solar_os_display_frame_t *frame);
