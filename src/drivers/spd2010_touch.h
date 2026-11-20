#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_SPD2010_TOUCH_ADDRESS 0x53U
typedef struct {
    esp_err_t (*write)(void *, const uint8_t *, size_t);
    esp_err_t (*read)(void *, uint8_t *, size_t);
    void (*delay_us)(void *, uint32_t);
    void *ctx;
} solar_os_spd2010_touch_io_t;
typedef struct {
    bool valid, touched;
    uint16_t x, y;
    uint8_t id;
} solar_os_spd2010_touch_sample_t;

/* Separate address-write/read transactions and their settling delays match
 * the SPD2010 touch protocol; a combined repeated-start read is not used. */
esp_err_t solar_os_spd2010_touch_probe(const solar_os_spd2010_touch_io_t *);
esp_err_t solar_os_spd2010_touch_read(const solar_os_spd2010_touch_io_t *,
    solar_os_spd2010_touch_sample_t *);
