#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define PCM5102_SAMPLE_RATE 16000U
#define PCM5102_FRAMES_PER_BLOCK 256U

esp_err_t pcm5102_open(int i2s_port, gpio_num_t bck_pin, gpio_num_t din_pin,
                       gpio_num_t rck_pin);
esp_err_t pcm5102_write_s16(const int16_t *samples, size_t frames,
                            uint8_t channels, uint8_t volume,
                            uint32_t timeout_ms, size_t *frames_written);
void pcm5102_close(void);
