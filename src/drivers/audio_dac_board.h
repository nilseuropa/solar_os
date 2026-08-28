#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE 16000U
#define AUDIO_DAC_BOARD_DEFAULT_CHANNELS 2U
#define AUDIO_DAC_BOARD_DEFAULT_BITS 16U
#define AUDIO_DAC_BOARD_DEFAULT_VOLUME 50U

typedef struct {
    bool initialized;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    int dac_pos_pin;
    int dac_neg_pin;
    int amp_pin;
    const char *output_codec;
    const char *input_codec;
} audio_dac_board_status_t;

typedef struct {
    int dac_pos_pin;
    int dac_neg_pin;
    int amp_pin;
    bool amp_active_high;
} audio_dac_board_config_t;

esp_err_t audio_dac_board_attach(const char *name,
                                 const audio_dac_board_config_t *config);
esp_err_t audio_dac_board_detach(const char *name);

esp_err_t audio_dac_board_init(void);
void audio_dac_board_deinit(void);
esp_err_t audio_dac_board_set_volume(uint8_t volume);
esp_err_t audio_dac_board_set_mic_gain(float gain_db);
esp_err_t audio_dac_board_write(const void *data, size_t len);
esp_err_t audio_dac_board_read(void *data, size_t len);
void audio_dac_board_get_status(audio_dac_board_status_t *status);
