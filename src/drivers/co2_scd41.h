#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef struct {
    float co2_ppm;
    float temperature_c;
    float humidity_pct;
    bool valid;
} co2_scd41_reading_t;

/*
 * Sensirion SCD41 CO2/temperature/humidity sensor on Port A's I2C bus
 * (see i2c_bus_port_a.h), fixed address 0x62. Starts the sensor's
 * periodic measurement mode once (command 0x21b1) -- without this it
 * stays idle and never has anything to report. New samples are ready
 * roughly every 5s afterward.
 */
esp_err_t co2_scd41_init(void);

/*
 * Reads the latest measurement (command 0xec05 + 12-byte read). Safe
 * to call more often than every 5s; it'll just re-read the same
 * sample until the sensor's next internal update. CRC bytes and the
 * sensor-status word in the response are read but not validated/used,
 * matching the reference this was ported from.
 */
esp_err_t co2_scd41_read(co2_scd41_reading_t *reading);
