#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "solar_os_display_mono_canvas.h"
#include "solar_os_display_maintenance.h"
#include "solar_os_display_geometry.h"
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
#define SOLAR_OS_DISPLAY_MODE_TEXT_MAX 1024
#define SOLAR_OS_DISPLAY_MODE_CHOICES_MAX 512
#define SOLAR_OS_DISPLAY_PRIMARY_TARGET "display0"

/* Only a display declared by the board profile belongs to the board-primary
 * path. A runtime display named display0 on a headless board remains a normal
 * display target so sessions and applications can discover it. */
bool solar_os_display_target_is_board_primary(const char *name);

typedef struct {
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
    char source[SOLAR_OS_DISPLAY_TARGET_SOURCE_MAX];
    char driver[SOLAR_OS_DISPLAY_TARGET_DRIVER_MAX];
    char controller[SOLAR_OS_DISPLAY_TARGET_CONTROLLER_MAX];
    char role[SOLAR_OS_DISPLAY_TARGET_ROLE_MAX];
    char owner[SOLAR_OS_DISPLAY_TARGET_OWNER_MAX];
    uint16_t width;
    uint16_t height;
    solar_os_display_rect_t panel, canvas;
    bool round;
    bool ready;
    bool brightness_supported;
    bool black_is_one;
    uint32_t surface_formats;
    uint32_t frame_formats;
    /* Working-buffer preference, independent of accepted transport formats.
     * Applications may use the compact monochrome canvas on a color-capable
     * target. The optional getter is sampled when taking a target snapshot. */
    bool prefer_mono;
    bool (*prefers_mono)(const void *context);
    void *render_context;
    uint16_t preferred_stream_fps;
    /* Minimum caller task stack for synchronous presentation, bytes; 0 uses
     * the presenter's default. Protocol-backed targets may nest substantial I/O. */
    uint16_t presentation_stack_bytes;
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
    esp_err_t (*present_power_state)(void *context);
    void *power_state_context;
    esp_err_t (*relax)(void *context, solar_os_display_relax_phase_t phase);
    void *relax_context;
    uint16_t native_width;
    uint16_t native_height;
    bool epaper;
    solar_os_display_mono_canvas_presenter_t present_canvas;
    void *canvas_context;
    /* Optional monochrome-to-color theme. Applied before registration becomes
     * visible, then under the presentation mutex on terminal color changes. */
    esp_err_t (*set_colors)(void *context, uint32_t foreground, uint32_t background);
    void *color_context;
} solar_os_display_target_t;

typedef struct {
    solar_os_display_rect_t native_canvas;
    uint32_t generation;
} solar_os_display_canvas_snapshot_t;

/* Runtime-only layout rectangle in the current panel's oriented/scaled pixels.
 * NULL restores the panel's default canvas. Pixel drawing may overflow it.
 * A snapshot preserves the exact native rectangle for a failed reflow. */
esp_err_t solar_os_display_set_canvas(const char *name, const solar_os_display_rect_t *canvas,
                                       solar_os_display_canvas_snapshot_t *previous);
esp_err_t solar_os_display_restore_canvas(const char *name,
                                           const solar_os_display_canvas_snapshot_t *previous);

typedef struct {
    uint32_t cycles;
    bool cancelled;
} solar_os_display_relax_result_t;

/* Exclusive, synchronous native black/white cycles under the caller's claim.
 * Cancellation is polled between hardware refreshes and during holds. END is
 * always attempted after a successful BEGIN, including after a frame error.
 */
esp_err_t solar_os_display_relax(const char *name, const char *owner, unsigned seconds,
                                 bool (*should_cancel)(void *), void *user,
                                 solar_os_display_relax_result_t *result,
                                 char *busy_owner, size_t busy_owner_len);

typedef enum {
    SOLAR_OS_DISPLAY_PRESENT_TEXT,
    SOLAR_OS_DISPLAY_PRESENT_GRAPHICS,
} solar_os_display_present_mode_t;

typedef struct {
    const uint8_t *data;
    size_t data_size;
    /* Optional native row-major RGB565 snapshot, most-significant byte first.
     * Rows contain native_width pixels without padding. Shares data's lifetime. */
    const uint8_t *rgb565_data;
    size_t rgb565_size;
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
/* Remove only the registration with both this name and its original canvas. */
esp_err_t solar_os_display_unregister_target_owned(const char *name, const u8g2_t *canvas);
/* Atomically removes all named targets, or none when any target is busy. */
esp_err_t solar_os_display_unregister_targets(const char *const *names,
                                              size_t count);
size_t solar_os_display_target_count(void);
bool solar_os_display_get_target(size_t index, solar_os_display_target_t *target);
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target);
bool solar_os_display_target_name_for_u8g2(const u8g2_t *u8g2,
                                           char *name,
                                           size_t name_len);
/* NULL selects display0. Unknown or unregistered displays return false. */
bool solar_os_display_is_epaper(const u8g2_t *u8g2);
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
/* A claimed streaming client can exclude terminal/status-bar painting without
 * changing the terminal's saved preferences. Release the block after joining
 * presentation. The epoch makes terminals repaint fully when drawing resumes. */
#define SOLAR_OS_DISPLAY_CLAIM_BLOCK_TERMINAL (1U << 0)
#define SOLAR_OS_DISPLAY_CLAIM_LOCK_ORIENTATION (1U << 1)
esp_err_t solar_os_display_set_claim_policy(const char *name, const char *owner,
                                           uint32_t mask, uint32_t values);
esp_err_t solar_os_display_set_terminal_blocked(const char *name, const char *owner, bool blocked);
esp_err_t solar_os_display_set_orientation_locked(const char *name, const char *owner, bool locked);
bool solar_os_display_terminal_draw_state(const u8g2_t *u8g2, uint32_t *epoch);
bool solar_os_display_brightness_supported(void);
esp_err_t solar_os_display_get_brightness(uint8_t *percent);
esp_err_t solar_os_display_set_brightness(uint8_t percent);
esp_err_t solar_os_display_get_theme(solar_os_display_theme_v1_t *theme);
esp_err_t solar_os_display_set_theme_color(solar_os_display_theme_role_t role, uint32_t rgb888);
esp_err_t solar_os_display_get_colors(uint32_t *foreground_rgb888,
                                      uint32_t *background_rgb888);
esp_err_t solar_os_display_set_foreground_color(uint32_t rgb888);
esp_err_t solar_os_display_set_background_color(uint32_t rgb888);
esp_err_t solar_os_display_suspend_primary(void);
esp_err_t solar_os_display_show_power_state(const char *status);
void solar_os_display_clear_power_state(void);
esp_err_t solar_os_display_shutdown(void);
esp_err_t solar_os_display_resume_primary(void);
bool solar_os_display_primary_suspended(void);
esp_err_t solar_os_display_set_palette_inverted(const char *name, bool inverted);
esp_err_t solar_os_display_get_controller_mode(const char *name, char *mode, size_t mode_capacity,
                                                char *values, size_t values_capacity);
esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode);
/* Non-persistent performance override; disabling restores driver settings. */
esp_err_t solar_os_display_set_high_refresh_override(const char *name,
                                                     bool enabled,
                                                     uint16_t hz_tenths);
/* The owner must keep its target claimed until disabling the override. */
esp_err_t solar_os_display_set_high_refresh_owned(const char *name,
                                                  const void *owner,
                                                  bool enabled,
                                                  uint16_t hz_tenths);
esp_err_t solar_os_display_set_overlay_active(u8g2_t *u8g2, bool active);
/* Live opaque layer, independent of the foreground display claim. Packed
 * horizontal bits are black when set. Coordinates use the logical display. */
esp_err_t solar_os_display_set_layer(const char *name, const char *owner,
    const uint8_t *bits, size_t size, uint16_t x, uint16_t y, uint16_t width, uint16_t height);
esp_err_t solar_os_display_clear_layer(const char *name, const char *owner);
bool solar_os_display_has_layer(const char *name, const char *owner);
void solar_os_display_present(u8g2_t *u8g2, solar_os_display_present_mode_t mode);
/* Areas use oriented/scaled panel pixels. Add the layout canvas origin when
 * drawing with canvas-relative coordinates. NULL/zero selects a full frame. */
void solar_os_display_present_regions(u8g2_t *u8g2, solar_os_display_present_mode_t mode,
    const solar_os_display_rect_t *areas, size_t count);
void solar_os_display_present_overlay(u8g2_t *u8g2,
                                      uint16_t x,
                                      uint16_t y,
                                      uint16_t width,
                                      uint16_t height,
                                      bool after_next_frame);
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
