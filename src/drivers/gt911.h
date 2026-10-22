#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "solar_os_gpio_controller.h"

#define GT911_ADDRESS 0x5dU
#define GT911_ALTERNATE_ADDRESS 0x14U

typedef struct {
    bool valid;
    bool home;
    bool touched;
    uint16_t x;
    uint16_t y;
    uint8_t id;
} gt911_sample_t;

esp_err_t gt911_init_with_reset(const char *i2c_bus, uint8_t address,
                                uint8_t alternate_address, int irq_pin,
                                int reset_pin, int power_pin, int power_active_level);

esp_err_t gt911_init_with_lines(const char *i2c_bus, uint8_t address,
    uint8_t alternate_address, int irq_pin,
    const solar_os_gpio_line_ref_t *reset, const solar_os_gpio_line_ref_t *power,
    int power_active_level);

esp_err_t gt911_init(const char *i2c_bus,
                     uint8_t address,
                     uint8_t alternate_address,
                     int irq_pin);
esp_err_t gt911_read(gt911_sample_t *sample);
void gt911_deinit(void);
