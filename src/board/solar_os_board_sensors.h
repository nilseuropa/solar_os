#pragma once

#include "esp_err.h"

typedef struct {
    float temperature_c;
    float humidity_percent;
} solar_os_board_environment_t;

esp_err_t solar_os_board_sensors_init(void);
esp_err_t solar_os_board_sensors_read_environment(solar_os_board_environment_t *environment);

