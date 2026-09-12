#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define GT911_ADDRESS 0x5dU
#define GT911_ALTERNATE_ADDRESS 0x14U

typedef struct {
    bool touched;
    uint16_t x;
    uint16_t y;
    uint8_t id;
} gt911_sample_t;

esp_err_t gt911_init(const char *i2c_bus, uint8_t address, int irq_pin);
esp_err_t gt911_read(gt911_sample_t *sample);
void gt911_deinit(void);
