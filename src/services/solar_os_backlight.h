#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Auxiliary illumination, independent of display brightness. Callbacks run
 * under the service mutex and must not call back into this service. */
typedef esp_err_t (*solar_os_backlight_set_fn_t)(void *user, uint8_t percent);
typedef struct {
    bool follow_display;
    bool suspended;
    uint8_t percent;          /* Configured level, or display level in follow mode. */
    uint8_t effective_percent;
    size_t providers;
    esp_err_t last_error;
} solar_os_backlight_status_t;

esp_err_t solar_os_backlight_register(solar_os_backlight_set_fn_t set, void *user);
/* Stops illumination and waits for in-flight callbacks before releasing user. */
void solar_os_backlight_unregister(void *user);
esp_err_t solar_os_backlight_get_status(solar_os_backlight_status_t *status);
esp_err_t solar_os_backlight_set_brightness(uint8_t percent);
esp_err_t solar_os_backlight_set_follow(void);
/* Toggle illumination for this boot without saving to NVS. A zero configured
 * level uses the default brightness when toggled on. Sleep preserves the choice. */
esp_err_t solar_os_backlight_toggle(void);
esp_err_t solar_os_backlight_display_changed(uint8_t percent);
esp_err_t solar_os_backlight_set_suspended(bool suspended);
