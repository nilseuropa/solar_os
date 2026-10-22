#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Waveshare MCU I/O-extension firmware protocol, not CH422G registers. */
typedef struct {
    esp_err_t (*read)(void *, uint8_t, uint8_t *, size_t);
    esp_err_t (*write)(void *, uint8_t, const uint8_t *, size_t);
    void *ctx;
} solar_os_ws_io_transport_t;

typedef struct {
    solar_os_ws_io_transport_t io;
    uint8_t output, direction; /* A set direction bit enables an output. */
    bool ready;
} solar_os_ws_io_chip_t;

esp_err_t solar_os_ws_io_chip_init(solar_os_ws_io_chip_t *, const solar_os_ws_io_transport_t *, uint8_t output, uint8_t direction);
esp_err_t solar_os_ws_io_chip_configure(solar_os_ws_io_chip_t *, uint8_t line, bool output);
esp_err_t solar_os_ws_io_chip_write(solar_os_ws_io_chip_t *, uint8_t line, bool level);
esp_err_t solar_os_ws_io_chip_read(solar_os_ws_io_chip_t *, uint8_t line, bool *level);
esp_err_t solar_os_ws_io_chip_pwm(solar_os_ws_io_chip_t *, uint8_t duty);
esp_err_t solar_os_ws_io_chip_adc(solar_os_ws_io_chip_t *, uint16_t *raw);
