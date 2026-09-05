#include "audio_codec_board.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "i2c_bus.h"
#include "soc/soc_caps.h"
#include "solar_os_audio.h"
#include "solar_os_audio_backend.h"

#define AUDIO_CODEC_DEVICE_ID_MAX 16U
#define AUDIO_CODEC_DMA_DESC_NUM 4
#define AUDIO_CODEC_DMA_FRAME_NUM 128
#define AUDIO_CODEC_TDM_SLOT_MASK \
    (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)

typedef struct {
    bool attached;
    bool output_initialized;
    bool input_initialized;
    bool tx_enabled;
    bool rx_enabled;
    bool volume_valid;
    bool mic_gain_valid;
    char id[AUDIO_CODEC_DEVICE_ID_MAX];
    audio_codec_board_config_t config;
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    const audio_codec_data_if_t *data_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_ctrl_if_t *out_ctrl_if;
    const audio_codec_ctrl_if_t *in_ctrl_if;
    const audio_codec_if_t *out_codec_if;
    const audio_codec_if_t *in_codec_if;
    esp_codec_dev_handle_t playback;
    esp_codec_dev_handle_t record;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t volume;
    float mic_gain_db;
} audio_codec_board_state_t;

static const char *TAG = "audio_codec";
static audio_codec_board_state_t audio_codec;

static bool audio_codec_is_duplex(void)
{
    return audio_codec.config.mode == AUDIO_CODEC_BOARD_ES8311_DUPLEX;
}

static void audio_codec_log_init_failure(esp_err_t ret)
{
    ESP_LOGW(TAG,
             "audio init failed: %s internal free=%u largest=%u dma free=%u largest=%u",
             esp_err_to_name(ret),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
}

static esp_err_t audio_codec_handle_init_error(esp_err_t ret)
{
    if (ret != ESP_OK) {
        audio_codec_log_init_failure(ret);
        audio_codec_board_deinit();
    }
    return ret;
}

static i2s_tdm_config_t audio_codec_i2s_tdm_config(void)
{
    i2s_tdm_config_t config = {
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            32, I2S_SLOT_MODE_STEREO, AUDIO_CODEC_TDM_SLOT_MASK),
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(
            AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE),
        .gpio_cfg = {
            .mclk = audio_codec.config.mclk_pin,
            .bclk = audio_codec.config.bclk_pin,
            .ws = audio_codec.config.ws_pin,
            .dout = audio_codec.config.dout_pin,
            .din = audio_codec.config.din_pin,
        },
    };
    config.slot_cfg.total_slot = 4;
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    return config;
}

static i2s_std_config_t audio_codec_i2s_std_config(void)
{
    i2s_std_config_t config = {
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            AUDIO_CODEC_BOARD_DEFAULT_BITS, I2S_SLOT_MODE_STEREO),
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(
            AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE),
        .gpio_cfg = {
            .mclk = audio_codec.config.mclk_pin,
            .bclk = audio_codec.config.bclk_pin,
            .ws = audio_codec.config.ws_pin,
            .dout = audio_codec.config.dout_pin,
            .din = audio_codec.config.din_pin,
        },
    };
    config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    return config;
}

static esp_err_t audio_codec_i2s_ensure_channels(void)
{
    if (audio_codec.tx_handle != NULL && audio_codec.rx_handle != NULL) {
        return ESP_OK;
    }
    if (audio_codec.tx_handle != NULL || audio_codec.rx_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    i2s_chan_config_t config = I2S_CHANNEL_DEFAULT_CONFIG(
        audio_codec.config.i2s_port, I2S_ROLE_MASTER);
    config.auto_clear = true;
    config.dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM;
    config.dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM;
    return i2s_new_channel(&config, &audio_codec.tx_handle,
                           &audio_codec.rx_handle);
}

static esp_err_t audio_codec_i2s_start(i2s_chan_handle_t handle)
{
    esp_err_t ret;
    if (audio_codec_is_duplex()) {
        i2s_std_config_t config = audio_codec_i2s_std_config();
        ret = i2s_channel_init_std_mode(handle, &config);
    } else {
        i2s_tdm_config_t config = audio_codec_i2s_tdm_config();
        ret = i2s_channel_init_tdm_mode(handle, &config);
    }
    return ret == ESP_OK ? i2s_channel_enable(handle) : ret;
}

static esp_err_t audio_codec_i2s_ensure_tx(void)
{
    if (audio_codec.tx_enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_channels(), TAG,
                        "create I2S channels failed");
    const esp_err_t ret = audio_codec_i2s_start(audio_codec.tx_handle);
    if (ret == ESP_OK) {
        audio_codec.tx_enabled = true;
    }
    return ret;
}

static esp_err_t audio_codec_i2s_ensure_rx(void)
{
    if (audio_codec.rx_enabled) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_channels(), TAG,
                        "create I2S channels failed");
    const esp_err_t ret = audio_codec_i2s_start(audio_codec.rx_handle);
    if (ret == ESP_OK) {
        audio_codec.rx_enabled = true;
    }
    return ret;
}

static esp_err_t audio_codec_ensure_common_interfaces(void)
{
    ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_channels(), TAG,
                        "create I2S channels failed");
    if (audio_codec.gpio_if == NULL) {
        audio_codec.gpio_if = audio_codec_new_gpio();
        if (audio_codec.gpio_if == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (audio_codec.data_if == NULL) {
        audio_codec_i2s_cfg_t config = {
            .port = audio_codec.config.i2s_port,
            .rx_handle = audio_codec.rx_handle,
            .tx_handle = audio_codec.tx_handle,
        };
        audio_codec.data_if = audio_codec_new_i2s_data(&config);
        if (audio_codec.data_if == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t audio_codec_ensure_output(void)
{
    if (audio_codec.output_initialized) {
        return ESP_OK;
    }
    if (!audio_codec.attached || audio_codec.config.i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_tx(), TAG,
                        "start I2S output failed");
    if (audio_codec_is_duplex()) {
        ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_rx(), TAG,
                            "start I2S input failed");
    }
    ESP_RETURN_ON_ERROR(audio_codec_ensure_common_interfaces(), TAG,
                        "create codec interfaces failed");

    esp_err_t ret = ESP_OK;
    i2c_bus_lock();
    audio_codec_i2c_cfg_t i2c = {
        .port = audio_codec.config.i2c_port,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = audio_codec.config.i2c_handle,
    };
    if (audio_codec.out_ctrl_if == NULL) {
        audio_codec.out_ctrl_if = audio_codec_new_i2c_ctrl(&i2c);
        if (audio_codec.out_ctrl_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (audio_codec.out_codec_if == NULL) {
        es8311_codec_cfg_t config = {
            .codec_mode = audio_codec_is_duplex() ?
                ESP_CODEC_DEV_WORK_MODE_BOTH : ESP_CODEC_DEV_WORK_MODE_DAC,
            .ctrl_if = audio_codec.out_ctrl_if,
            .gpio_if = audio_codec.gpio_if,
            .pa_pin = audio_codec.config.pa_pin,
            .pa_reverted = audio_codec_is_duplex(),
            .use_mclk = true,
            .hw_gain.pa_gain = 6.0f,
        };
        audio_codec.out_codec_if = es8311_codec_new(&config);
        if (audio_codec.out_codec_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (audio_codec.playback == NULL) {
        esp_codec_dev_cfg_t config = {
            .dev_type = audio_codec_is_duplex() ?
                ESP_CODEC_DEV_TYPE_IN_OUT : ESP_CODEC_DEV_TYPE_OUT,
            .codec_if = audio_codec.out_codec_if,
            .data_if = audio_codec.data_if,
        };
        audio_codec.playback = esp_codec_dev_new(&config);
        if (audio_codec.playback == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE,
        .channel = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS,
        .bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS,
    };
    if (esp_codec_dev_open(audio_codec.playback, &sample) != ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }
    const uint8_t volume = audio_codec.volume_valid ? audio_codec.volume :
        AUDIO_CODEC_BOARD_DEFAULT_VOLUME;
    if (esp_codec_dev_set_out_vol(audio_codec.playback, volume) !=
        ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }
    audio_codec.sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    audio_codec.channels = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    audio_codec.bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS;
    audio_codec.volume = volume;
    audio_codec.volume_valid = true;
    audio_codec.output_initialized = true;
out:
    i2c_bus_unlock();
    return ret;
}

static esp_err_t audio_codec_ensure_input(void)
{
    if (audio_codec.input_initialized) {
        return ESP_OK;
    }
    if (audio_codec_is_duplex()) {
        ESP_RETURN_ON_ERROR(audio_codec_ensure_output(), TAG,
                            "start duplex codec failed");
        const float gain = audio_codec.mic_gain_valid ?
            audio_codec.mic_gain_db : AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB;
        i2c_bus_lock();
        const esp_err_t ret =
            esp_codec_dev_set_in_gain(audio_codec.playback, gain) ==
                ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
        i2c_bus_unlock();
        if (ret == ESP_OK) {
            audio_codec.record = audio_codec.playback;
            audio_codec.mic_gain_db = gain;
            audio_codec.mic_gain_valid = true;
            audio_codec.input_initialized = true;
        }
        return ret;
    }

    ESP_RETURN_ON_ERROR(audio_codec_i2s_ensure_rx(), TAG,
                        "start I2S input failed");
    ESP_RETURN_ON_ERROR(audio_codec_ensure_common_interfaces(), TAG,
                        "create codec interfaces failed");
    esp_err_t ret = ESP_OK;
    i2c_bus_lock();
    audio_codec_i2c_cfg_t i2c = {
        .port = audio_codec.config.i2c_port,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = audio_codec.config.i2c_handle,
    };
    if (audio_codec.in_ctrl_if == NULL) {
        audio_codec.in_ctrl_if = audio_codec_new_i2c_ctrl(&i2c);
        if (audio_codec.in_ctrl_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (audio_codec.in_codec_if == NULL) {
        es7210_codec_cfg_t config = {
            .ctrl_if = audio_codec.in_ctrl_if,
            .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                            ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
        };
        audio_codec.in_codec_if = es7210_codec_new(&config);
        if (audio_codec.in_codec_if == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    if (audio_codec.record == NULL) {
        esp_codec_dev_cfg_t config = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN,
            .codec_if = audio_codec.in_codec_if,
            .data_if = audio_codec.data_if,
        };
        audio_codec.record = esp_codec_dev_new(&config);
        if (audio_codec.record == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto out;
        }
    }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE,
        .channel = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS,
        .bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS,
    };
    if (esp_codec_dev_open(audio_codec.record, &sample) != ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }
    const float gain = audio_codec.mic_gain_valid ? audio_codec.mic_gain_db :
        AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB;
    if (esp_codec_dev_set_in_gain(audio_codec.record, gain) !=
        ESP_CODEC_DEV_OK) {
        ret = ESP_FAIL;
        goto out;
    }
    audio_codec.sample_rate = AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE;
    audio_codec.channels = AUDIO_CODEC_BOARD_DEFAULT_CHANNELS;
    audio_codec.bits_per_sample = AUDIO_CODEC_BOARD_DEFAULT_BITS;
    audio_codec.mic_gain_db = gain;
    audio_codec.mic_gain_valid = true;
    audio_codec.input_initialized = true;
out:
    i2c_bus_unlock();
    return ret;
}

esp_err_t audio_codec_board_init(void)
{
    if (!audio_codec.attached) {
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_codec.output_initialized && audio_codec.input_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = audio_codec_ensure_output();
    if (ret == ESP_OK) {
        ret = audio_codec_ensure_input();
    }
    if (ret != ESP_OK) {
        return audio_codec_handle_init_error(ret);
    }
    ESP_LOGI(TAG,
             "audio ready: %s I2S%d mclk=%d bclk=%d ws=%d din=%d dout=%d pa=%d",
             audio_codec_is_duplex() ? "ES8311 duplex" : "ES8311/ES7210",
             audio_codec.config.i2s_port, audio_codec.config.mclk_pin,
             audio_codec.config.bclk_pin, audio_codec.config.ws_pin,
             audio_codec.config.din_pin, audio_codec.config.dout_pin,
             audio_codec.config.pa_pin);
    return ESP_OK;
}

void audio_codec_board_deinit(void)
{
    if (audio_codec.playback != NULL) {
        esp_codec_dev_close(audio_codec.playback);
        esp_codec_dev_delete(audio_codec.playback);
    }
    if (!audio_codec_is_duplex() && audio_codec.record != NULL) {
        esp_codec_dev_close(audio_codec.record);
        esp_codec_dev_delete(audio_codec.record);
    }
    if (audio_codec.in_codec_if != NULL) audio_codec_delete_codec_if(audio_codec.in_codec_if);
    if (audio_codec.out_codec_if != NULL) audio_codec_delete_codec_if(audio_codec.out_codec_if);
    if (audio_codec.in_ctrl_if != NULL) audio_codec_delete_ctrl_if(audio_codec.in_ctrl_if);
    if (audio_codec.out_ctrl_if != NULL) audio_codec_delete_ctrl_if(audio_codec.out_ctrl_if);
    if (audio_codec.data_if != NULL) audio_codec_delete_data_if(audio_codec.data_if);
    if (audio_codec.gpio_if != NULL) audio_codec_delete_gpio_if(audio_codec.gpio_if);
    if (audio_codec.tx_enabled) (void)i2s_channel_disable(audio_codec.tx_handle);
    if (audio_codec.tx_handle != NULL) (void)i2s_del_channel(audio_codec.tx_handle);
    if (audio_codec.rx_enabled) (void)i2s_channel_disable(audio_codec.rx_handle);
    if (audio_codec.rx_handle != NULL) (void)i2s_del_channel(audio_codec.rx_handle);

    const bool attached = audio_codec.attached;
    char id[AUDIO_CODEC_DEVICE_ID_MAX];
    strlcpy(id, audio_codec.id, sizeof(id));
    const audio_codec_board_config_t config = audio_codec.config;
    const bool volume_valid = audio_codec.volume_valid;
    const uint8_t volume = audio_codec.volume;
    const bool gain_valid = audio_codec.mic_gain_valid;
    const float gain = audio_codec.mic_gain_db;
    memset(&audio_codec, 0, sizeof(audio_codec));
    audio_codec.attached = attached;
    strlcpy(audio_codec.id, id, sizeof(audio_codec.id));
    audio_codec.config = config;
    audio_codec.volume_valid = volume_valid;
    audio_codec.volume = volume;
    audio_codec.mic_gain_valid = gain_valid;
    audio_codec.mic_gain_db = gain;
}

esp_err_t audio_codec_board_set_volume(uint8_t volume)
{
    if (volume > 100U) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = audio_codec_ensure_output();
    if (ret != ESP_OK) return audio_codec_handle_init_error(ret);
    i2c_bus_lock();
    ret = esp_codec_dev_set_out_vol(audio_codec.playback, volume) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    i2c_bus_unlock();
    if (ret == ESP_OK) {
        audio_codec.volume = volume;
        audio_codec.volume_valid = true;
    }
    return ret;
}

esp_err_t audio_codec_board_set_mic_gain(float gain_db)
{
    esp_err_t ret = audio_codec_ensure_input();
    if (ret != ESP_OK) return audio_codec_handle_init_error(ret);
    i2c_bus_lock();
    ret = esp_codec_dev_set_in_gain(audio_codec.record, gain_db) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
    i2c_bus_unlock();
    if (ret == ESP_OK) {
        audio_codec.mic_gain_db = gain_db;
        audio_codec.mic_gain_valid = true;
    }
    return ret;
}

esp_err_t audio_codec_board_write(const void *data, size_t len)
{
    if (data == NULL || len == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = audio_codec_ensure_output();
    if (ret != ESP_OK) return audio_codec_handle_init_error(ret);
    return esp_codec_dev_write(audio_codec.playback, (void *)data, (int)len) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t audio_codec_board_read(void *data, size_t len)
{
    if (data == NULL || len == 0U) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = audio_codec_ensure_input();
    if (ret != ESP_OK) return audio_codec_handle_init_error(ret);
    return esp_codec_dev_read(audio_codec.record, data, (int)len) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void audio_codec_board_get_status(audio_codec_board_status_t *status)
{
    if (status == NULL) return;
    *status = (audio_codec_board_status_t) {
        .initialized = audio_codec.output_initialized || audio_codec.input_initialized,
        .sample_rate = audio_codec.sample_rate != 0U ? audio_codec.sample_rate : AUDIO_CODEC_BOARD_DEFAULT_SAMPLE_RATE,
        .channels = audio_codec.channels != 0U ? audio_codec.channels : AUDIO_CODEC_BOARD_DEFAULT_CHANNELS,
        .bits_per_sample = audio_codec.bits_per_sample != 0U ? audio_codec.bits_per_sample : AUDIO_CODEC_BOARD_DEFAULT_BITS,
        .volume = audio_codec.volume_valid ? audio_codec.volume : AUDIO_CODEC_BOARD_DEFAULT_VOLUME,
        .mic_gain_db = audio_codec.mic_gain_valid ? audio_codec.mic_gain_db : AUDIO_CODEC_BOARD_DEFAULT_MIC_GAIN_DB,
        .i2s_port = audio_codec.config.i2s_port,
        .mclk_pin = audio_codec.config.mclk_pin,
        .bclk_pin = audio_codec.config.bclk_pin,
        .ws_pin = audio_codec.config.ws_pin,
        .din_pin = audio_codec.config.din_pin,
        .dout_pin = audio_codec.config.dout_pin,
        .pa_pin = audio_codec.config.pa_pin,
        .output_codec = "ES8311",
        .input_codec = audio_codec_is_duplex() ? "ES8311" : "ES7210",
    };
}

static esp_err_t backend_init(void *context) { return context == &audio_codec ? audio_codec_board_init() : ESP_ERR_INVALID_ARG; }
static void backend_deinit(void *context) { if (context == &audio_codec) audio_codec_board_deinit(); }
static esp_err_t backend_volume(void *context, uint8_t value) { return context == &audio_codec ? audio_codec_board_set_volume(value) : ESP_ERR_INVALID_ARG; }
static esp_err_t backend_gain(void *context, float value) { return context == &audio_codec ? audio_codec_board_set_mic_gain(value) : ESP_ERR_INVALID_ARG; }
static esp_err_t backend_write(void *context, const void *data, size_t len) { return context == &audio_codec ? audio_codec_board_write(data, len) : ESP_ERR_INVALID_ARG; }
static esp_err_t backend_read(void *context, void *data, size_t len) { return context == &audio_codec ? audio_codec_board_read(data, len) : ESP_ERR_INVALID_ARG; }

static void backend_status(void *context, solar_os_audio_backend_status_t *status)
{
    if (context != &audio_codec || status == NULL) return;
    audio_codec_board_status_t driver;
    audio_codec_board_get_status(&driver);
    *status = (solar_os_audio_backend_status_t) {
        .initialized = driver.initialized, .sample_rate = driver.sample_rate,
        .channels = driver.channels, .bits_per_sample = driver.bits_per_sample,
        .volume = driver.volume, .mic_gain_db = driver.mic_gain_db,
        .i2s_port = driver.i2s_port, .mclk_pin = driver.mclk_pin,
        .bclk_pin = driver.bclk_pin, .ws_pin = driver.ws_pin,
        .din_pin = driver.din_pin, .dout_pin = driver.dout_pin,
        .pa_pin = driver.pa_pin, .output_codec = driver.output_codec,
        .input_codec = driver.input_codec,
    };
}

static void backend_info(void *context, solar_os_audio_backend_info_t *info)
{
    if (context == &audio_codec && info != NULL) {
        *info = (solar_os_audio_backend_info_t) {
            .id = audio_codec.id,
            .name = audio_codec_is_duplex() ? "ES8311 duplex audio" : "ES8311/ES7210 audio",
            .has_input = true,
        };
    }
}

static const solar_os_audio_backend_ops_t backend_ops = {
    .init = backend_init, .deinit = backend_deinit,
    .set_volume = backend_volume, .set_mic_gain = backend_gain,
    .write = backend_write, .read = backend_read,
    .get_status = backend_status, .get_info = backend_info,
};

static bool audio_codec_config_valid(const audio_codec_board_config_t *config)
{
    return config != NULL && config->i2c_handle != NULL &&
        config->i2c_port >= 0 && config->i2s_port >= 0 && config->i2s_port < SOC_I2S_NUM &&
        GPIO_IS_VALID_OUTPUT_GPIO(config->mclk_pin) &&
        GPIO_IS_VALID_OUTPUT_GPIO(config->bclk_pin) &&
        GPIO_IS_VALID_OUTPUT_GPIO(config->ws_pin) &&
        GPIO_IS_VALID_GPIO(config->din_pin) &&
        GPIO_IS_VALID_OUTPUT_GPIO(config->dout_pin) &&
        /* pa is optional: boards whose amplifier enable sits behind an I/O
         * expander (e.g. LilyGO T-LoRa-Pager) pass -1 and manage it themselves;
         * esp_codec_dev treats a negative pa_pin as "none". */
        (config->pa_pin < 0 || GPIO_IS_VALID_OUTPUT_GPIO(config->pa_pin));
}

esp_err_t audio_codec_board_attach(const char *name, const audio_codec_board_config_t *config)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, sizeof(audio_codec.id)) >= sizeof(audio_codec.id) || !audio_codec_config_valid(config)) return ESP_ERR_INVALID_ARG;
    if (audio_codec.attached) return ESP_ERR_NOT_ALLOWED;
    memset(&audio_codec, 0, sizeof(audio_codec));
    audio_codec.attached = true;
    strlcpy(audio_codec.id, name, sizeof(audio_codec.id));
    audio_codec.config = *config;
    esp_err_t ret = solar_os_audio_backend_attach(&backend_ops, &audio_codec);
    if (ret == ESP_OK) ret = solar_os_audio_register_streams();
    if (ret != ESP_OK) {
        (void)solar_os_audio_backend_detach(&audio_codec);
        memset(&audio_codec, 0, sizeof(audio_codec));
    }
    return ret;
}

esp_err_t audio_codec_board_detach(const char *name)
{
    if (name == NULL || !audio_codec.attached || strcmp(name, audio_codec.id) != 0) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = solar_os_audio_unregister_streams(name);
    if (ret != ESP_OK) return ret;
    audio_codec_board_deinit();
    ret = solar_os_audio_backend_detach(&audio_codec);
    if (ret == ESP_OK) memset(&audio_codec, 0, sizeof(audio_codec));
    return ret;
}
