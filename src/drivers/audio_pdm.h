#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_PDM_SAMPLE_RATE 16000U
#define AUDIO_PDM_FRAMES_PER_BLOCK 256U

esp_err_t audio_pdm_prepare_power(int power_pin);
esp_err_t audio_pdm_open(int clk_pin, int data_pin, int power_pin, unsigned slot);
esp_err_t audio_pdm_read(void *data, size_t len, uint32_t timeout_ms, size_t *read_len);
esp_err_t audio_pdm_close(void);
