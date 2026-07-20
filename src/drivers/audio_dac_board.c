#include "audio_dac_board.h"

#include <string.h>

#include "driver/dac_continuous.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "solar_os_board.h"

#define AUDIO_DAC_DESC_NUM 8U
#define AUDIO_DAC_DMA_BUFFER_BYTES 1024U
#define AUDIO_DAC_CONVERT_BUFFER_BYTES 2048U
#define AUDIO_DAC_FRAMES_PER_WRITE 256U
#define AUDIO_DAC_INPUT_FRAME_BYTES \
    ((AUDIO_DAC_BOARD_DEFAULT_CHANNELS * AUDIO_DAC_BOARD_DEFAULT_BITS) / 8U)
#define AUDIO_DAC_WRITE_TIMEOUT_MS 1000
#define AUDIO_DAC_MIDPOINT 128U
#define AUDIO_DAC_TARGET_QUEUED_US 32000LL

#ifndef SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN
#define SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN GPIO_NUM_NC
#endif

#ifndef SOLAR_OS_BOARD_AUDIO_AMP_EN_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_AUDIO_AMP_EN_ACTIVE_LEVEL 1
#endif

typedef struct {
    bool initialized;
    bool volume_set;
    dac_continuous_handle_t handle;
    uint8_t *buffer;
    uint8_t volume;
    int64_t playback_until_us;
} audio_dac_board_state_t;

static const char *TAG = "audio_dac";
static audio_dac_board_state_t audio_dac;

static bool audio_dac_is_pin(gpio_num_t pin)
{
    return pin == GPIO_NUM_25 || pin == GPIO_NUM_26;
}

static dac_channel_mask_t audio_dac_channel_mask(void)
{
    dac_channel_mask_t mask = 0;
    if (SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS == GPIO_NUM_25) {
        mask |= DAC_CHANNEL_MASK_CH0;
    } else if (SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS == GPIO_NUM_26) {
        mask |= DAC_CHANNEL_MASK_CH1;
    }
    if (SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG == GPIO_NUM_25) {
        mask |= DAC_CHANNEL_MASK_CH0;
    } else if (SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG == GPIO_NUM_26) {
        mask |= DAC_CHANNEL_MASK_CH1;
    }
    return mask;
}

static uint8_t audio_dac_output_channels(void)
{
    return audio_dac_is_pin(SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG) ? 2U : 1U;
}

static uint8_t audio_dac_output_samples_per_frame(void)
{
    return audio_dac_output_channels();
}

static uint32_t audio_dac_output_rate(void)
{
    return AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE * audio_dac_output_samples_per_frame();
}

static dac_continuous_digi_clk_src_t audio_dac_clock_source(void)
{
    return DAC_DIGI_CLK_SRC_APLL;
}

static void audio_dac_set_amp_enabled(bool enabled)
{
    if (SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN == GPIO_NUM_NC) {
        return;
    }

    const int active = SOLAR_OS_BOARD_AUDIO_AMP_EN_ACTIVE_LEVEL ? 1 : 0;
    gpio_set_level(SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN, enabled ? active : !active);
}

static esp_err_t audio_dac_init_amp(void)
{
    if (SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN == GPIO_NUM_NC) {
        return ESP_OK;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret == ESP_OK) {
        audio_dac_set_amp_enabled(false);
    }
    return ret;
}

static void audio_dac_board_close(bool write_silence)
{
#if SOC_DAC_SUPPORTED
    if (audio_dac.handle != NULL) {
        if (write_silence && audio_dac.buffer != NULL) {
            memset(audio_dac.buffer, AUDIO_DAC_MIDPOINT, AUDIO_DAC_DMA_BUFFER_BYTES);
            (void)dac_continuous_write(audio_dac.handle,
                                       audio_dac.buffer,
                                       AUDIO_DAC_DMA_BUFFER_BYTES,
                                       NULL,
                                       50);
        }
        (void)dac_continuous_disable(audio_dac.handle);
        (void)dac_continuous_del_channels(audio_dac.handle);
    }
#else
    (void)write_silence;
#endif
    if (audio_dac.buffer != NULL) {
        heap_caps_free(audio_dac.buffer);
    }
    audio_dac.handle = NULL;
    audio_dac.buffer = NULL;
    audio_dac.initialized = false;
    audio_dac.playback_until_us = 0;
    audio_dac_set_amp_enabled(false);
}

static void audio_dac_board_recover_after_write_error(esp_err_t ret)
{
    ESP_LOGW(TAG, "DAC write failed: %s, resetting DAC channel", esp_err_to_name(ret));
    audio_dac_board_close(false);
}

static uint8_t audio_dac_current_volume(void)
{
    if (!audio_dac.volume_set) {
        return AUDIO_DAC_BOARD_DEFAULT_VOLUME;
    }
    return audio_dac.volume;
}

static uint8_t audio_dac_sample_to_u8(int32_t sample)
{
    if (sample < -128) {
        sample = -128;
    } else if (sample > 127) {
        sample = 127;
    }
    return (uint8_t)(sample + (int32_t)AUDIO_DAC_MIDPOINT);
}

static size_t audio_dac_convert_frames(const int16_t *input, size_t frames, uint8_t *output)
{
    const uint8_t volume = audio_dac_current_volume();
    const uint8_t output_channels = audio_dac_output_channels();
    const uint8_t output_samples = audio_dac_output_samples_per_frame();

    for (size_t frame = 0; frame < frames; frame++) {
        const int32_t left = input[(frame * AUDIO_DAC_BOARD_DEFAULT_CHANNELS) + 0];
        const int32_t right = input[(frame * AUDIO_DAC_BOARD_DEFAULT_CHANNELS) + 1];
        int32_t mixed = (left + right) / 2;
        mixed = (mixed * (int32_t)volume) / 100;
        const int32_t sample8 = mixed >> 8;

        output[(frame * output_samples) + 0U] = audio_dac_sample_to_u8(sample8);
        if (output_channels > 1U) {
            output[(frame * output_samples) + 1U] = audio_dac_sample_to_u8(-sample8);
        } else {
            for (uint8_t sample = 1; sample < output_samples; sample++) {
                output[(frame * output_samples) + sample] = output[(frame * output_samples) + 0U];
            }
        }
    }

    return frames * output_samples;
}

static void audio_dac_delay_us(int64_t delay_us)
{
    if (delay_us <= 0) {
        return;
    }
    TickType_t ticks = pdMS_TO_TICKS((uint32_t)((delay_us + 999LL) / 1000LL));
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static int64_t audio_dac_frames_to_us(size_t frames)
{
    return ((int64_t)frames * 1000000LL) / (int64_t)AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE;
}

static void audio_dac_pace_before_frames(size_t frames)
{
    int64_t now_us = esp_timer_get_time();
    if (audio_dac.playback_until_us < now_us) {
        audio_dac.playback_until_us = now_us;
    }

    int64_t queued_us = audio_dac.playback_until_us - now_us;
    const int64_t frame_us = audio_dac_frames_to_us(frames);
    if (queued_us + frame_us > AUDIO_DAC_TARGET_QUEUED_US) {
        audio_dac_delay_us((queued_us + frame_us) - AUDIO_DAC_TARGET_QUEUED_US);
    }
}

static void audio_dac_note_frames_queued(size_t frames)
{
    const int64_t now_us = esp_timer_get_time();
    if (audio_dac.playback_until_us < now_us) {
        audio_dac.playback_until_us = now_us;
    }
    audio_dac.playback_until_us += audio_dac_frames_to_us(frames);
}

esp_err_t audio_dac_board_init(void)
{
#if !SOC_DAC_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (audio_dac.initialized) {
        return ESP_OK;
    }

    if (!audio_dac.volume_set) {
        audio_dac.volume = AUDIO_DAC_BOARD_DEFAULT_VOLUME;
        audio_dac.volume_set = true;
    }

    esp_err_t ret = audio_dac_init_amp();
    if (ret != ESP_OK) {
        return ret;
    }

    const dac_channel_mask_t channel_mask = audio_dac_channel_mask();
    ESP_RETURN_ON_FALSE(channel_mask != 0, ESP_ERR_NOT_SUPPORTED, TAG, "no DAC output channel");

    /* The continuous DAC consumes this hot-path buffer from internal memory. */
    audio_dac.buffer = heap_caps_malloc(AUDIO_DAC_CONVERT_BUFFER_BYTES,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(audio_dac.buffer != NULL, ESP_ERR_NO_MEM, TAG, "no DAC buffer");

    dac_continuous_config_t config = {
        .chan_mask = channel_mask,
        .desc_num = AUDIO_DAC_DESC_NUM,
        .buf_size = AUDIO_DAC_DMA_BUFFER_BYTES,
        .freq_hz = audio_dac_output_rate(),
        .offset = 0,
        .clk_src = audio_dac_clock_source(),
        .chan_mode = audio_dac_output_channels() > 1U ?
            DAC_CHANNEL_MODE_ALTER :
            DAC_CHANNEL_MODE_SIMUL,
    };

    ret = dac_continuous_new_channels(&config, &audio_dac.handle);
    if (ret == ESP_OK) {
        ret = dac_continuous_enable(audio_dac.handle);
    }
    if (ret != ESP_OK) {
        audio_dac_board_deinit();
        return ret;
    }

    audio_dac.playback_until_us = esp_timer_get_time();
    audio_dac.initialized = true;
    audio_dac_set_amp_enabled(true);
    ESP_LOGI(TAG,
             "audio ready: %s dac pos=%d neg=%d channels=%u rate=%u volume=%u",
             SOLAR_OS_BOARD_AUDIO_CODEC_OUT,
             (int)SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS,
             (int)SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG,
             (unsigned)audio_dac_output_channels(),
             AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE,
             (unsigned)audio_dac.volume);
    return ESP_OK;
#endif
}

void audio_dac_board_deinit(void)
{
    audio_dac_board_close(true);
}

esp_err_t audio_dac_board_set_volume(uint8_t volume)
{
    if (volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_dac.volume = volume;
    audio_dac.volume_set = true;
    if (volume == 0) {
        audio_dac_board_deinit();
        return ESP_OK;
    }
    return audio_dac_board_init();
}

esp_err_t audio_dac_board_set_mic_gain(float gain_db)
{
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_dac_board_write(const void *data, size_t len)
{
    if (data == NULL || len == 0 || (len % AUDIO_DAC_INPUT_FRAME_BYTES) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t frames_remaining = len / AUDIO_DAC_INPUT_FRAME_BYTES;
    if (audio_dac_current_volume() == 0) {
        audio_dac_delay_us(audio_dac_frames_to_us(frames_remaining));
        return ESP_OK;
    }

    esp_err_t ret = audio_dac_board_init();
    if (ret != ESP_OK) {
        return ret;
    }

    const int16_t *input = (const int16_t *)data;
    const size_t max_convert_frames =
        AUDIO_DAC_CONVERT_BUFFER_BYTES / audio_dac_output_samples_per_frame();

    while (frames_remaining > 0) {
        size_t frames_per_chunk = AUDIO_DAC_FRAMES_PER_WRITE;
        if (frames_per_chunk > max_convert_frames) {
            frames_per_chunk = max_convert_frames;
        }
        const size_t frames = frames_remaining > frames_per_chunk ?
            frames_per_chunk :
            frames_remaining;
        const size_t output_bytes = audio_dac_convert_frames(input, frames, audio_dac.buffer);
        size_t bytes_loaded = 0;
        audio_dac_pace_before_frames(frames);
        ret = dac_continuous_write(audio_dac.handle,
                                   audio_dac.buffer,
                                   output_bytes,
                                   &bytes_loaded,
                                   AUDIO_DAC_WRITE_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "DAC write failed after loading %u/%u bytes",
                     (unsigned)bytes_loaded,
                     (unsigned)output_bytes);
            audio_dac_board_recover_after_write_error(ret);
            return ret;
        }
        audio_dac_note_frames_queued(frames);
        input += frames * AUDIO_DAC_BOARD_DEFAULT_CHANNELS;
        frames_remaining -= frames;
    }

    return ESP_OK;
}

esp_err_t audio_dac_board_read(void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_dac_board_get_status(audio_dac_board_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->initialized = audio_dac.initialized;
    status->sample_rate = AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE;
    status->channels = AUDIO_DAC_BOARD_DEFAULT_CHANNELS;
    status->bits_per_sample = AUDIO_DAC_BOARD_DEFAULT_BITS;
    status->volume = audio_dac_current_volume();
    status->dac_pos_pin = SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS;
    status->dac_neg_pin = SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG;
    status->output_codec = SOLAR_OS_BOARD_AUDIO_CODEC_OUT;
    status->input_codec = SOLAR_OS_BOARD_AUDIO_CODEC_IN;
}
