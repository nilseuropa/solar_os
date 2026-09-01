#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define PCM1808_SAMPLE_RATE 16000U
#define PCM1808_CHANNELS 2U
#define PCM1808_BITS_PER_SAMPLE 16U
#define PCM1808_FRAMES_PER_BLOCK 256U

int pcm1808_i2s_port(void);
esp_err_t pcm1808_open(gpio_num_t mclk_pin, gpio_num_t bck_pin,
                       gpio_num_t ws_pin, gpio_num_t dout_pin);
esp_err_t pcm1808_read_s16(int16_t *samples, size_t frames,
                           uint32_t timeout_ms, size_t *frames_read);
void pcm1808_close(void);
