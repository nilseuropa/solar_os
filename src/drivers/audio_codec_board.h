#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE 16000U
#define AUDIO_CODEC_BOARD_DEFAULT_CHANNELS 2U
#define AUDIO_CODEC_BOARD_DEFAULT_BITS 16U
#define AUDIO_CODEC_BOARD_DEFAULT_VOLUME 50U
#define AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB 35.0f

typedef struct {
    bool initialized;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    float mic_gain_db;
    int i2s_port;
    int mclk_pin;
    int bclk_pin;
    int ws_pin;
    int din_pin;
    int dout_pin;
    int pa_pin;
    const char *output_codec;
    const char *input_codec;
} audio_codec_board_status_t;

typedef enum {
    AUDIO_CODEC_BOARD_ES8311_ES7210,
    AUDIO_CODEC_BOARD_ES8311_DUPLEX,
} audio_codec_board_mode_t;

typedef struct {
    audio_codec_board_mode_t mode;
    i2c_master_bus_handle_t i2c_handle;
    int i2c_port;
    int i2s_port;
    int mclk_pin;
    int bclk_pin;
    int ws_pin;
    int din_pin;
    int dout_pin;
    int pa_pin;
} audio_codec_board_config_t;

esp_err_t audio_codec_board_attach(const char *name,
                                   const audio_codec_board_config_t *config);
esp_err_t audio_codec_board_detach(const char *name);

esp_err_t audio_codec_board_init(void);
void audio_codec_board_deinit(void);
esp_err_t audio_codec_board_set_volume(uint8_t volume);
esp_err_t audio_codec_board_set_mic_gain(float gain_db);
esp_err_t audio_codec_board_write(const void *data, size_t len);
esp_err_t audio_codec_board_read(void *data, size_t len);
void audio_codec_board_get_status(audio_codec_board_status_t *status);
