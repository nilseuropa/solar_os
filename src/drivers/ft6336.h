#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    bool touched;
    uint8_t id;
    uint16_t x;
    uint16_t y;
} ft6336_sample_t;

esp_err_t ft6336_init(void);
esp_err_t ft6336_read(ft6336_sample_t *sample);
void ft6336_deinit(void);
