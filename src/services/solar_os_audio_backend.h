#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    const char *id;
    const char *name;
    bool has_input;
} solar_os_audio_backend_info_t;

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
} solar_os_audio_backend_status_t;

typedef struct {
    esp_err_t (*init)(void *context);
    void (*deinit)(void *context);
    esp_err_t (*set_volume)(void *context, uint8_t volume);
    esp_err_t (*set_mic_gain)(void *context, float gain_db);
    esp_err_t (*write)(void *context, const void *data, size_t len);
    esp_err_t (*read)(void *context, void *data, size_t len);
    void (*get_status)(void *context, solar_os_audio_backend_status_t *status);
    void (*get_info)(void *context, solar_os_audio_backend_info_t *info);
} solar_os_audio_backend_ops_t;

esp_err_t solar_os_audio_backend_attach(const solar_os_audio_backend_ops_t *ops,
                                        void *context);
esp_err_t solar_os_audio_backend_detach(void *context);
bool solar_os_audio_backend_available(void);
bool solar_os_audio_backend_has_input(void);
esp_err_t solar_os_audio_backend_init(void);
void solar_os_audio_backend_deinit(void);
esp_err_t solar_os_audio_backend_set_volume(uint8_t volume);
esp_err_t solar_os_audio_backend_set_mic_gain(float gain_db);
esp_err_t solar_os_audio_backend_write(const void *data, size_t len);
esp_err_t solar_os_audio_backend_read(void *data, size_t len);
void solar_os_audio_backend_get_status(solar_os_audio_backend_status_t *status);
void solar_os_audio_backend_get_info(solar_os_audio_backend_info_t *info);
