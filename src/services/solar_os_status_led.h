#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t solar_os_status_led_init(void);
esp_err_t solar_os_status_led_set(bool on);
esp_err_t solar_os_status_led_toggle(bool *on_after);
esp_err_t solar_os_status_led_get(bool *on);
