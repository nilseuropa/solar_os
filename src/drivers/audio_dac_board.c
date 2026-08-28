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
#include "solar_os_audio.h"
#include "solar_os_audio_backend.h"

#define AUDIO_DAC_DESC_NUM 8U
#define AUDIO_DAC_DMA_BUFFER_BYTES 1024U
#define AUDIO_DAC_CONVERT_BUFFER_BYTES 2048U
#define AUDIO_DAC_FRAMES_PER_WRITE 256U
#define AUDIO_DAC_INPUT_FRAME_BYTES \
    ((AUDIO_DAC_BOARD_DEFAULT_CHANNELS * AUDIO_DAC_BOARD_DEFAULT_BITS) / 8U)
#define AUDIO_DAC_WRITE_TIMEOUT_MS 1000
#define AUDIO_DAC_MIDPOINT 128U
#define AUDIO_DAC_TARGET_QUEUED_US 32000LL

typedef struct {
    bool attached;
    bool initialized;
    bool volume_set;
    char id[16];
    audio_dac_board_config_t config;
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
    if (audio_dac.config.dac_pos_pin == GPIO_NUM_25) {
        mask |= DAC_CHANNEL_MASK_CH0;
    } else if (audio_dac.config.dac_pos_pin == GPIO_NUM_26) {
        mask |= DAC_CHANNEL_MASK_CH1;
    }
    if (audio_dac.config.dac_neg_pin == GPIO_NUM_25) {
        mask |= DAC_CHANNEL_MASK_CH0;
    } else if (audio_dac.config.dac_neg_pin == GPIO_NUM_26) {
        mask |= DAC_CHANNEL_MASK_CH1;
    }
    return mask;
}

static uint8_t audio_dac_output_channels(void)
{
    return audio_dac_is_pin(audio_dac.config.dac_neg_pin) ? 2U : 1U;
}

static uint8_t audio_dac_output_samples_per_frame(void)
{
    /*
     * PLL_D2 requires at least 19.6 kHz on ESP32. Repeat mono samples so the
     * 16 kHz input stream drives the DAC at 32 kHz without changing pitch or
     * duration. Stereo already emits two samples per input frame.
     */
    return 2U;
}

static uint32_t audio_dac_output_rate(void)
{
    return AUDIO_DAC_BOARD_DEFAULT_SAMPLE_RATE * audio_dac_output_samples_per_frame();
}

static dac_continuous_digi_clk_src_t audio_dac_clock_source(void)
{
    /* Keep the shared APLL available for timing-sensitive display drivers. */
    return DAC_DIGI_CLK_SRC_DEFAULT;
}

static void audio_dac_set_amp_enabled(bool enabled)
{
    if (audio_dac.config.amp_pin >= 0) {
        const int active = audio_dac.config.amp_active_high ? 1 : 0;
        gpio_set_level(audio_dac.config.amp_pin, enabled ? active : !active);
    }
}

static esp_err_t audio_dac_init_amp(void)
{
    if (audio_dac.config.amp_pin < 0) {
        return ESP_OK;
    }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << audio_dac.config.amp_pin,
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
    if (!audio_dac.attached) {
        return ESP_ERR_INVALID_STATE;
    }
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
             "ESP32-DAC",
             audio_dac.config.dac_pos_pin,
             audio_dac.config.dac_neg_pin,
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
    status->dac_pos_pin = audio_dac.config.dac_pos_pin;
    status->dac_neg_pin = audio_dac.config.dac_neg_pin;
    status->amp_pin = audio_dac.config.amp_pin;
    status->output_codec = "ESP32-DAC";
    status->input_codec = "-";
}

static esp_err_t dac_backend_init(void *context)
{
    return context == &audio_dac ? audio_dac_board_init() : ESP_ERR_INVALID_ARG;
}

static void dac_backend_deinit(void *context)
{
    if (context == &audio_dac) audio_dac_board_deinit();
}

static esp_err_t dac_backend_volume(void *context, uint8_t volume)
{
    return context == &audio_dac ? audio_dac_board_set_volume(volume) : ESP_ERR_INVALID_ARG;
}

static esp_err_t dac_backend_gain(void *context, float gain_db)
{
    return context == &audio_dac ? audio_dac_board_set_mic_gain(gain_db) : ESP_ERR_INVALID_ARG;
}

static esp_err_t dac_backend_write(void *context, const void *data, size_t len)
{
    return context == &audio_dac ? audio_dac_board_write(data, len) : ESP_ERR_INVALID_ARG;
}

static void dac_backend_status(void *context,
                               solar_os_audio_backend_status_t *status)
{
    if (context != &audio_dac || status == NULL) return;
    audio_dac_board_status_t driver;
    audio_dac_board_get_status(&driver);
    *status = (solar_os_audio_backend_status_t) {
        .initialized = driver.initialized,
        .sample_rate = driver.sample_rate,
        .channels = driver.channels,
        .bits_per_sample = driver.bits_per_sample,
        .volume = driver.volume,
        .i2s_port = -1,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .ws_pin = -1,
        .din_pin = driver.dac_neg_pin,
        .dout_pin = driver.dac_pos_pin,
        .pa_pin = driver.amp_pin,
        .output_codec = driver.output_codec,
        .input_codec = driver.input_codec,
    };
}

static void dac_backend_info(void *context,
                             solar_os_audio_backend_info_t *info)
{
    if (context == &audio_dac && info != NULL) {
        *info = (solar_os_audio_backend_info_t) {
            .id = audio_dac.id,
            .name = "ESP32 DAC audio",
            .has_input = false,
        };
    }
}

static const solar_os_audio_backend_ops_t dac_backend_ops = {
    .init = dac_backend_init,
    .deinit = dac_backend_deinit,
    .set_volume = dac_backend_volume,
    .set_mic_gain = dac_backend_gain,
    .write = dac_backend_write,
    .get_status = dac_backend_status,
    .get_info = dac_backend_info,
};

esp_err_t audio_dac_board_attach(const char *name,
                                 const audio_dac_board_config_t *config)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, sizeof(audio_dac.id)) >= sizeof(audio_dac.id) ||
        config == NULL || !audio_dac_is_pin(config->dac_pos_pin) ||
        (config->dac_neg_pin >= 0 && !audio_dac_is_pin(config->dac_neg_pin)) ||
        config->dac_pos_pin == config->dac_neg_pin ||
        (config->amp_pin >= 0 && !GPIO_IS_VALID_OUTPUT_GPIO(config->amp_pin))) {
        return ESP_ERR_INVALID_ARG;
    }
    if (audio_dac.attached) {
        return ESP_ERR_NOT_ALLOWED;
    }
    memset(&audio_dac, 0, sizeof(audio_dac));
    audio_dac.attached = true;
    strlcpy(audio_dac.id, name, sizeof(audio_dac.id));
    audio_dac.config = *config;
    esp_err_t ret = solar_os_audio_backend_attach(&dac_backend_ops, &audio_dac);
    if (ret == ESP_OK) {
        ret = solar_os_audio_register_streams();
    }
    if (ret != ESP_OK) {
        (void)solar_os_audio_backend_detach(&audio_dac);
        memset(&audio_dac, 0, sizeof(audio_dac));
    }
    return ret;
}

esp_err_t audio_dac_board_detach(const char *name)
{
    if (name == NULL || !audio_dac.attached || strcmp(name, audio_dac.id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t ret = solar_os_audio_unregister_streams(name);
    if (ret != ESP_OK) {
        return ret;
    }
    audio_dac_board_deinit();
    ret = solar_os_audio_backend_detach(&audio_dac);
    if (ret == ESP_OK) {
        memset(&audio_dac, 0, sizeof(audio_dac));
    }
    return ret;
}
