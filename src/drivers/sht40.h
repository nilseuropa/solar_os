#pragma once

#include <stdint.h>

#include "esp_err.h"

#define SHT40_ADDRESS 0x44
#define SHT40_BUS_NAME_MAX 16

typedef struct {
    char bus[SHT40_BUS_NAME_MAX];
    uint8_t address;
} sht40_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
    uint16_t id;
} sht40_measurement_t;

esp_err_t sht40_init(void);
esp_err_t sht40_read_id(uint16_t *id);
esp_err_t sht40_read_measurement(sht40_measurement_t *measurement);
esp_err_t sht40_init_device(sht40_t *device, const char *bus, uint8_t address);
esp_err_t sht40_read_id_device(const sht40_t *device, uint16_t *id);
esp_err_t sht40_read_measurement_device(const sht40_t *device,
                                        sht40_measurement_t *measurement);
