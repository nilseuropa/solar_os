#include "audio_aw88298_board.h"

#include <string.h>

#include "aw88298_dac.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "io_expander_aw9523b.h"
#include "solar_os_board.h"

#define AUDIO_AW88298_DMA_DESC_NUM 4
#define AUDIO_AW88298_DMA_FRAME_NUM 128

typedef struct {
    bool output_initialized;
    bool tx_enabled;
    bool volume_valid;
    i2s_chan_handle_t tx_handle;
    const audio_codec_data_if_t *data_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_ctrl_if_t *out_ctrl_if;
    const audio_codec_if_t *out_codec_if;
    esp_codec_dev_handle_t playback;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
} audio_aw88298_board_state_t;

static const char *TAG = "audio_aw88298";
static audio_aw88298_board_state_t audio_aw88298;

static void audio_aw88298_log_init_failure(esp_err_t ret)
{
    ESP_LOGW(TAG,
             "audio init failed: %s internal free=%u largest=%u dma free=%u largest=%u",
             esp_err_to_name(ret),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}

static esp_err_t audio_aw88298_handle_init_error(esp_err_t ret)
{
    if (ret != ESP_OK) {
        audio_aw88298_log_init_failure(ret);
        audio_aw88298_board_deinit();
    }
    return ret;
}

static esp_err_t audio_aw88298_i2s_ensure_tx(void)
{
    if (audio_aw88298.tx_enabled) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(SOLAR_OS_BOARD_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    chan_cfg.dma_desc_num = AUDIO_AW88298_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = AUDIO_AW88298_DMA_FRAME_NUM;
    esp_err_t ret = i2s_new_channel(&chan_cfg, &audio_aw88298.tx_handle, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_AW88298_BOARD_DEFAULT_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = SOLAR_OS_BOARD_PIN_I2S_MCLK,
            .bclk = SOLAR_OS_BOARD_PIN_I2S_BCLK,
            .ws = SOLAR_OS_BOARD_PIN_I2S_WS,
            .dout = SOLAR_OS_BOARD_PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ret = i2s_channel_init_std_mode(audio_aw88298.tx_handle, &std_cfg);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = i2s_channel_enable(audio_aw88298.tx_handle);
    if (ret != ESP_OK) {
        return ret;
    }

    audio_aw88298.tx_enabled = true;
    return ESP_OK;
}

static esp_err_t audio_aw88298_ensure_output(void)
{
    if (audio_aw88298.output_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = audio_aw88298_i2s_ensure_tx();
    if (ret != ESP_OK) {
        return ret;
    }

    if (audio_aw88298.gpio_if == NULL) {
        audio_aw88298.gpio_if = audio_codec_new_gpio();
        if (audio_aw88298.gpio_if == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (audio_aw88298.data_if == NULL) {
        audio_codec_i2s_cfg_t i2s_cfg = {
            .port = SOLAR_OS_BOARD_I2S_PORT,
            .rx_handle = NULL,
            .tx_handle = audio_aw88298.tx_handle,
        };
        audio_aw88298.data_if = audio_codec_new_i2s_data(&i2s_cfg);
        if (audio_aw88298.data_if == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    i2c_master_bus_handle_t i2c_handle = i2c_bus_get_handle();
    if (i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Amp power on before talking to it over I2C, matching M5Stack's own
     * enable-before-configure ordering (M5Unified's
     * _speaker_enabled_cb_cores3). io_expander_aw9523b_set_speaker_enable()
     * locks the i2c bus itself, so this must happen before i2c_bus_lock()
     * below -- the bus mutex isn't recursive. */
    ret = io_expander_aw9523b_set_speaker_enable(true);
    if (ret != ESP_OK) {
        return ret;
    }

    i2c_bus_lock();

    audio_codec_i2c_cfg_t out_i2c = {
        .port = SOLAR_OS_BOARD_I2C_PORT,
        .addr = AW88298_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    if (audio_aw88298.out_ctrl_if == NULL) {
        audio_aw88298.out_ctrl_if = audio_codec_new_i2c_ctrl(&out_i2c);
        if (audio_aw88298.out_ctrl_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }

    if (audio_aw88298.out_codec_if == NULL) {
        aw88298_codec_cfg_t aw88298_cfg = {
            .ctrl_if = audio_aw88298.out_ctrl_if,
            .gpio_if = audio_aw88298.gpio_if,
            .reset_pin = -1,
        };
        audio_aw88298.out_codec_if = aw88298_codec_new(&aw88298_cfg);
        if (audio_aw88298.out_codec_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }

    if (audio_aw88298.playback == NULL) {
        esp_codec_dev_cfg_t playback_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = audio_aw88298.out_codec_if,
            .data_if = audio_aw88298.data_if,
        };
        audio_aw88298.playback = esp_codec_dev_new(&playback_cfg);
        if (audio_aw88298.playback == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }

    esp_codec_dev_sample_info_t sample_info = {
        .sample_rate = AUDIO_AW88298_BOARD_DEFAULT_SAMPLE_RATE,
        .channel = AUDIO_AW88298_BOARD_DEFAULT_CHANNELS,
        .bits_per_sample = AUDIO_AW88298_BOARD_DEFAULT_BITS,
    };
    if (esp_codec_dev_open(audio_aw88298.playback, &sample_info) != ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }

    const uint8_t volume = audio_aw88298.volume_valid ?
        audio_aw88298.volume :
        AUDIO_AW88298_BOARD_DEFAULT_VOLUME;
    if (esp_codec_dev_set_out_vol(audio_aw88298.playback, volume) != ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }

    audio_aw88298.sample_rate = AUDIO_AW88298_BOARD_DEFAULT_SAMPLE_RATE;
    audio_aw88298.channels = AUDIO_AW88298_BOARD_DEFAULT_CHANNELS;
    audio_aw88298.bits_per_sample = AUDIO_AW88298_BOARD_DEFAULT_BITS;
    audio_aw88298.volume = volume;
    audio_aw88298.volume_valid = true;
    audio_aw88298.output_initialized = true;

out:
    i2c_bus_unlock();
    if (ret != ESP_OK) {
        (void)io_expander_aw9523b_set_speaker_enable(false);
    }
    return ret;
}

esp_err_t audio_aw88298_board_init(void)
{
    if (audio_aw88298.output_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = audio_aw88298_ensure_output();
    if (ret != ESP_OK) {
        return audio_aw88298_handle_init_error(ret);
    }

    ESP_LOGI(TAG,
             "audio ready: %s I2S%d bclk=%d ws=%d dout=%d",
             SOLAR_OS_BOARD_AUDIO_CODEC_OUT,
             (int)SOLAR_OS_BOARD_I2S_PORT,
             (int)SOLAR_OS_BOARD_PIN_I2S_BCLK,
             (int)SOLAR_OS_BOARD_PIN_I2S_WS,
             (int)SOLAR_OS_BOARD_PIN_I2S_DOUT);
    return ESP_OK;
}

void audio_aw88298_board_deinit(void)
{
    if (audio_aw88298.playback != NULL) {
        esp_codec_dev_close(audio_aw88298.playback);
        esp_codec_dev_delete(audio_aw88298.playback);
    }
    if (audio_aw88298.out_codec_if != NULL) {
        audio_codec_delete_codec_if(audio_aw88298.out_codec_if);
    }
    if (audio_aw88298.out_ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(audio_aw88298.out_ctrl_if);
    }
    if (audio_aw88298.data_if != NULL) {
        audio_codec_delete_data_if(audio_aw88298.data_if);
    }
    if (audio_aw88298.gpio_if != NULL) {
        audio_codec_delete_gpio_if(audio_aw88298.gpio_if);
    }
    if (audio_aw88298.tx_enabled) {
        i2s_channel_disable(audio_aw88298.tx_handle);
        audio_aw88298.tx_enabled = false;
    }
    if (audio_aw88298.tx_handle != NULL) {
        i2s_del_channel(audio_aw88298.tx_handle);
    }
    if (audio_aw88298.output_initialized) {
        (void)io_expander_aw9523b_set_speaker_enable(false);
    }

    memset(&audio_aw88298, 0, sizeof(audio_aw88298));
}

esp_err_t audio_aw88298_board_set_volume(uint8_t volume)
{
    if (volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = audio_aw88298_ensure_output();
    if (ret != ESP_OK) {
        return audio_aw88298_handle_init_error(ret);
    }

    i2c_bus_lock();
    ret = esp_codec_dev_set_out_vol(audio_aw88298.playback, volume) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
    i2c_bus_unlock();
    if (ret == ESP_OK) {
        audio_aw88298.volume = volume;
    }
    return ret;
}

esp_err_t audio_aw88298_board_set_mic_gain(float gain_db)
{
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_aw88298_board_write(const void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = audio_aw88298_ensure_output();
    if (ret != ESP_OK) {
        return audio_aw88298_handle_init_error(ret);
    }
    return esp_codec_dev_write(audio_aw88298.playback, (void *)data, (int)len) == ESP_CODEC_DEV_OK ?
        ESP_OK :
        ESP_FAIL;
}

esp_err_t audio_aw88298_board_read(void *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

void audio_aw88298_board_get_status(audio_aw88298_board_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->initialized = audio_aw88298.output_initialized;
    status->sample_rate = audio_aw88298.sample_rate != 0 ?
        audio_aw88298.sample_rate :
        AUDIO_AW88298_BOARD_DEFAULT_SAMPLE_RATE;
    status->channels = audio_aw88298.channels != 0 ?
        audio_aw88298.channels :
        AUDIO_AW88298_BOARD_DEFAULT_CHANNELS;
    status->bits_per_sample = audio_aw88298.bits_per_sample != 0 ?
        audio_aw88298.bits_per_sample :
        AUDIO_AW88298_BOARD_DEFAULT_BITS;
    status->volume = audio_aw88298.volume_valid ? audio_aw88298.volume : AUDIO_AW88298_BOARD_DEFAULT_VOLUME;
    status->i2s_port = SOLAR_OS_BOARD_I2S_PORT;
    status->mclk_pin = SOLAR_OS_BOARD_PIN_I2S_MCLK;
    status->bclk_pin = SOLAR_OS_BOARD_PIN_I2S_BCLK;
    status->ws_pin = SOLAR_OS_BOARD_PIN_I2S_WS;
    status->din_pin = SOLAR_OS_BOARD_PIN_I2S_DIN;
    status->dout_pin = SOLAR_OS_BOARD_PIN_I2S_DOUT;
    status->pa_pin = -1;
    status->output_codec = SOLAR_OS_BOARD_AUDIO_CODEC_OUT;
    status->input_codec = SOLAR_OS_BOARD_AUDIO_CODEC_IN;
}
