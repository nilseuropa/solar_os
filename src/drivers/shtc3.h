#pragma once

#include <stdint.h>

#include "esp_err.h"

#define SHTC3_ADDRESS 0x70
#define SHTC3_BUS_NAME_MAX 16

typedef struct {
    char bus[SHTC3_BUS_NAME_MAX];
    uint8_t address;
} shtc3_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
    uint16_t id;
} shtc3_measurement_t;

esp_err_t shtc3_init(void);
esp_err_t shtc3_read_id(uint16_t *id);
esp_err_t shtc3_read_measurement(shtc3_measurement_t *measurement);
esp_err_t shtc3_init_device(shtc3_t *device, const char *bus, uint8_t address);
esp_err_t shtc3_read_id_device(const shtc3_t *device, uint16_t *id);
esp_err_t shtc3_read_measurement_device(const shtc3_t *device,
                                        shtc3_measurement_t *measurement);
