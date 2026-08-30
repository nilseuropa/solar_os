#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_display_surface.h"
#include "u8g2.h"

typedef struct solar_os_board_display {
    const struct solar_os_board_display_ops *ops;
    void *driver;
    const char *driver_name;
    u8g2_t *u8g2;
    const char *controller;
    uint16_t width;
    uint16_t height;
    uint32_t surface_formats;
    uint32_t frame_formats;
    uint16_t preferred_stream_fps;
    uint32_t max_stream_pixels_per_second;
    bool ready;
} solar_os_board_display_t;

typedef struct solar_os_board_display_ops {
    esp_err_t (*runtime_ready)(solar_os_board_display_t *display);
    esp_err_t (*resume)(solar_os_board_display_t *display);
    void (*deinit)(solar_os_board_display_t *display);
    bool (*brightness_supported)(const solar_os_board_display_t *display);
    esp_err_t (*get_brightness)(const solar_os_board_display_t *display,
                                uint8_t *percent);
    esp_err_t (*set_brightness)(solar_os_board_display_t *display,
                                uint8_t percent);
    esp_err_t (*set_colors)(solar_os_board_display_t *display,
                            uint32_t foreground_rgb888,
                            uint32_t background_rgb888);
    const char *(*controller_mode)(const solar_os_board_display_t *display);
    const char *(*controller_mode_values)(const solar_os_board_display_t *display);
    esp_err_t (*set_controller_mode)(solar_os_board_display_t *display,
                                     const char *mode);
    esp_err_t (*set_high_refresh_override)(solar_os_board_display_t *display,
                                           bool enabled,
                                           uint16_t hz_tenths);
    esp_err_t (*present_mono_xbm)(solar_os_board_display_t *display,
                                  const uint8_t *bitmap,
                                  size_t bitmap_size,
                                  uint16_t x,
                                  uint16_t y,
                                  uint16_t width,
                                  uint16_t height,
                                  uint16_t stride,
                                  bool palette_inverted);
    esp_err_t (*present_surface)(solar_os_board_display_t *display,
                                 const solar_os_display_surface_t *surface);
    esp_err_t (*present_frame)(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame);
} solar_os_board_display_ops_t;

esp_err_t solar_os_board_display_register_primary(solar_os_board_display_t *display);
esp_err_t solar_os_board_display_unregister_primary(solar_os_board_display_t *display);

esp_err_t solar_os_board_display_init(solar_os_board_display_t *display);
esp_err_t solar_os_board_display_runtime_ready(solar_os_board_display_t *display);
esp_err_t solar_os_board_display_resume(solar_os_board_display_t *display);
void solar_os_board_display_deinit(solar_os_board_display_t *display);
u8g2_t *solar_os_board_display_u8g2(solar_os_board_display_t *display);
const char *solar_os_board_display_driver_name(const solar_os_board_display_t *display);
const char *solar_os_board_display_controller(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_width(const solar_os_board_display_t *display);
uint16_t solar_os_board_display_height(const solar_os_board_display_t *display);
bool solar_os_board_display_ready(const solar_os_board_display_t *display);
uint32_t solar_os_board_display_surface_formats(
    const solar_os_board_display_t *display);
uint32_t solar_os_board_display_frame_formats(
    const solar_os_board_display_t *display);
uint16_t solar_os_board_display_preferred_stream_fps(
    const solar_os_board_display_t *display);
uint32_t solar_os_board_display_max_stream_pixels_per_second(
    const solar_os_board_display_t *display);
bool solar_os_board_display_brightness_supported(const solar_os_board_display_t *display);
esp_err_t solar_os_board_display_get_brightness(const solar_os_board_display_t *display,
                                                uint8_t *percent);
esp_err_t solar_os_board_display_set_brightness(solar_os_board_display_t *display,
                                                uint8_t percent);
esp_err_t solar_os_board_display_set_colors(solar_os_board_display_t *display,
                                            uint32_t foreground_rgb888,
                                            uint32_t background_rgb888);
const char *solar_os_board_display_controller_mode(const solar_os_board_display_t *display);
const char *solar_os_board_display_controller_mode_values(const solar_os_board_display_t *display);
esp_err_t solar_os_board_display_set_controller_mode(solar_os_board_display_t *display,
                                                     const char *mode);
esp_err_t solar_os_board_display_set_high_refresh_override(
    solar_os_board_display_t *display,
    bool enabled,
    uint16_t hz_tenths);
esp_err_t solar_os_board_display_present_mono_xbm(solar_os_board_display_t *display,
                                                  const uint8_t *bitmap,
                                                  size_t bitmap_size,
                                                  uint16_t x,
                                                  uint16_t y,
                                                  uint16_t width,
                                                  uint16_t height,
                                                  uint16_t stride,
                                                  bool palette_inverted);
esp_err_t solar_os_board_display_present_surface(
    solar_os_board_display_t *display,
    const solar_os_display_surface_t *surface);
esp_err_t solar_os_board_display_present_frame(
    solar_os_board_display_t *display,
    const solar_os_display_raster_t *frame);
