#pragma once

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_INMP441_SAMPLE_RATE 16000U
#define SOLAR_OS_INMP441_BLOCK_FRAMES 256U

typedef struct {
    int port, bck, ws, din, dout;
    int power; /* Optional active-high DAC/amp rail enable; -1 if absent. */
    unsigned slot;
} solar_os_inmp441_config_t;

typedef struct {
    solar_os_inmp441_config_t config;
    SemaphoreHandle_t lock;
    i2s_chan_handle_t rx, tx;
    bool rx_enabled, tx_enabled, capture_open, playback_open;
} solar_os_inmp441_t;

esp_err_t solar_os_inmp441_init(solar_os_inmp441_t *device,
                                const solar_os_inmp441_config_t *config);
void solar_os_inmp441_deinit(solar_os_inmp441_t *device);
esp_err_t solar_os_inmp441_open(solar_os_inmp441_t *device, bool capture);
void solar_os_inmp441_close(solar_os_inmp441_t *device, bool capture);
esp_err_t solar_os_inmp441_read(solar_os_inmp441_t *device, int16_t *samples,
                                size_t frames, uint32_t timeout_ms,
                                size_t *read);
esp_err_t solar_os_inmp441_write(solar_os_inmp441_t *device,
                                 const int16_t *samples, size_t frames,
                                 unsigned channels, uint8_t volume,
                                 uint32_t timeout_ms, size_t *written);
