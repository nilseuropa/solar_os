#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_gfx.h"
#include "u8g2.h"

typedef struct solar_os_board_display solar_os_board_display_t;

#define SOLAR_OS_DISPLAY_TARGET_MAX 6
#define SOLAR_OS_DISPLAY_TARGET_NAME_MAX 16
#define SOLAR_OS_DISPLAY_TARGET_SOURCE_MAX 12
#define SOLAR_OS_DISPLAY_TARGET_DRIVER_MAX 20
#define SOLAR_OS_DISPLAY_TARGET_CONTROLLER_MAX 20
#define SOLAR_OS_DISPLAY_TARGET_ROLE_MAX 16
#define SOLAR_OS_DISPLAY_TARGET_OWNER_MAX 32

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
    u8g2_t *u8g2;
} solar_os_display_target_t;

typedef enum {
    SOLAR_OS_DISPLAY_PRESENT_TEXT,
    SOLAR_OS_DISPLAY_PRESENT_GRAPHICS,
    SOLAR_OS_DISPLAY_PRESENT_LOW_SHIMMER,
} solar_os_display_present_mode_t;

esp_err_t solar_os_display_init(solar_os_board_display_t *display);
esp_err_t solar_os_display_register_target(const solar_os_display_target_t *target);
esp_err_t solar_os_display_unregister_target(const char *name);
size_t solar_os_display_target_count(void);
bool solar_os_display_get_target(size_t index, solar_os_display_target_t *target);
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target);
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
esp_err_t solar_os_display_get_controller_mode(const char *name,
                                               const char **mode,
                                               const char **values);
esp_err_t solar_os_display_set_controller_mode(const char *name, const char *mode);
esp_err_t solar_os_display_request_present_mode(u8g2_t *u8g2,
                                                solar_os_display_present_mode_t mode);
