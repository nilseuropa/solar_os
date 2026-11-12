#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define IP2315_I2C_ADDR_DEFAULT 0x75U
#define IP2315_REG_CHARGE_EN 0x01U
#define IP2315_REG_CHG_STAT 0xC7U
#define IP2315_CHG_STAT_CHARGING 0x80U

typedef struct {
    esp_err_t (*read)(void *ctx, uint8_t reg, uint8_t *data, size_t len);
    esp_err_t (*write)(void *ctx, uint8_t reg, const uint8_t *data, size_t len);
    void *ctx;
} ip2315_io_t;

typedef struct {
    ip2315_io_t io;
    bool initialized;
} ip2315_t;

esp_err_t ip2315_init(ip2315_t *device, const ip2315_io_t *io);
esp_err_t ip2315_deinit(ip2315_t *device);
esp_err_t ip2315_set_charge_enabled(ip2315_t *device, bool enabled);
esp_err_t ip2315_read_charge_stat(ip2315_t *device, uint8_t *stat);
