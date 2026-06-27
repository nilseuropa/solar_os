#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint16_t battery_mv;
    bool calibrated;
} solar_os_board_battery_sample_t;

esp_err_t solar_os_board_battery_init(void);
esp_err_t solar_os_board_battery_read(solar_os_board_battery_sample_t *sample);
