#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool pressed;
    uint8_t pointer_id;
    uint16_t x;
    uint16_t y;
} solar_os_board_pointer_sample_t;

esp_err_t solar_os_board_pointer_init(void);
esp_err_t solar_os_board_pointer_read(solar_os_board_pointer_sample_t *sample);
void solar_os_board_pointer_deinit(void);
