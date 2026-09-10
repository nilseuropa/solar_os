#include "solar_os_es7210.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "i2c_bus.h"
#include "soc/soc_caps.h"
#include "solar_os_audio.h"
#include "solar_os_buses.h"
#include "solar_os_stream.h"

#define ES7210_DEVICE_NAME_MAX 16U
#define ES7210_SAMPLE_RATE 16000U
#define ES7210_CHANNELS 2U
#define ES7210_BITS_PER_SAMPLE 16U
#define ES7210_FRAMES_PER_BLOCK 128U
#define ES7210_DMA_DESC_NUM 4U
#define ES7210_TDM_SLOT_MASK \
    (I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3)
#define ES7210_DEFAULT_GAIN_DB 0.0f

typedef struct {
    bool attached;
    bool initialized;
    bool rx_enabled;
    bool gain_valid;
    int i2s_port;
    int i2c_port;
    int mclk_pin;
    int bck_pin;
    int ws_pin;
    int din_pin;
    i2c_master_bus_handle_t i2c_handle;
    i2s_chan_handle_t rx_handle;
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_if_t *codec_if;
    esp_codec_dev_handle_t record;
    float gain_db;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char stream_id[SOLAR_OS_STREAM_ID_MAX];
} solar_os_es7210_device_t;

static solar_os_es7210_device_t es7210_audio;

static const solar_os_stream_audio_format_t es7210_native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = ES7210_SAMPLE_RATE,
    .channels = ES7210_CHANNELS,
    .bits_per_sample = ES7210_BITS_PER_SAMPLE,
    .frames_per_block = ES7210_FRAMES_PER_BLOCK,
};

static bool es7210_requested_format_supported(
    const solar_os_stream_audio_format_t *format)
{
    return format == NULL ||
        ((format->sample_rate == 0U || format->sample_rate == ES7210_SAMPLE_RATE) &&
         (format->channels == 0U || format->channels == ES7210_CHANNELS) &&
         (format->bits_per_sample == 0U || format->bits_per_sample == ES7210_BITS_PER_SAMPLE) &&
         format->sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE);
}

static void es7210_deinit(void)
{
    if (es7210_audio.record != NULL) {
        (void)esp_codec_dev_close(es7210_audio.record);
        esp_codec_dev_delete(es7210_audio.record);
    }
    if (es7210_audio.codec_if != NULL) audio_codec_delete_codec_if(es7210_audio.codec_if);
    if (es7210_audio.ctrl_if != NULL) audio_codec_delete_ctrl_if(es7210_audio.ctrl_if);
    if (es7210_audio.data_if != NULL) audio_codec_delete_data_if(es7210_audio.data_if);
    if (es7210_audio.rx_enabled) (void)i2s_channel_disable(es7210_audio.rx_handle);
    if (es7210_audio.rx_handle != NULL) (void)i2s_del_channel(es7210_audio.rx_handle);

    es7210_audio.initialized = false;
    es7210_audio.rx_enabled = false;
    es7210_audio.rx_handle = NULL;
    es7210_audio.data_if = NULL;
    es7210_audio.ctrl_if = NULL;
    es7210_audio.codec_if = NULL;
    es7210_audio.record = NULL;
}

static esp_err_t es7210_init(void)
{
    if (es7210_audio.initialized) return ESP_OK;
    if (!es7210_audio.attached || es7210_audio.i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        es7210_audio.i2s_port, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = ES7210_DMA_DESC_NUM;
    channel_config.dma_frame_num = ES7210_FRAMES_PER_BLOCK;
    esp_err_t err = i2s_new_channel(&channel_config, NULL, &es7210_audio.rx_handle);
    if (err != ESP_OK) goto fail;

    i2s_tdm_config_t tdm_config = {
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(
            32, I2S_SLOT_MODE_STEREO, ES7210_TDM_SLOT_MASK),
        .clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(ES7210_SAMPLE_RATE),
        .gpio_cfg = {
            .mclk = es7210_audio.mclk_pin,
            .bclk = es7210_audio.bck_pin,
            .ws = es7210_audio.ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = es7210_audio.din_pin,
        },
    };
    tdm_config.slot_cfg.total_slot = 4;
    tdm_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    err = i2s_channel_init_tdm_mode(es7210_audio.rx_handle, &tdm_config);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(es7210_audio.rx_handle);
    if (err != ESP_OK) goto fail;
    es7210_audio.rx_enabled = true;

    audio_codec_i2s_cfg_t data_config = {
        .port = es7210_audio.i2s_port,
        .rx_handle = es7210_audio.rx_handle,
    };
    es7210_audio.data_if = audio_codec_new_i2s_data(&data_config);
    if (es7210_audio.data_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    i2c_bus_lock();
    audio_codec_i2c_cfg_t i2c_config = {
        .port = es7210_audio.i2c_port,
        .addr = ES7210_CODEC_DEFAULT_ADDR,
        .bus_handle = es7210_audio.i2c_handle,
    };
    es7210_audio.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (es7210_audio.ctrl_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    es7210_codec_cfg_t codec_config = {
        .ctrl_if = es7210_audio.ctrl_if,
        .mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                        ES7210_SEL_MIC3 | ES7210_SEL_MIC4,
    };
    es7210_audio.codec_if = es7210_codec_new(&codec_config);
    if (es7210_audio.codec_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7210_audio.codec_if,
        .data_if = es7210_audio.data_if,
    };
    es7210_audio.record = esp_codec_dev_new(&device_config);
    if (es7210_audio.record == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = ES7210_SAMPLE_RATE,
        .channel = ES7210_CHANNELS,
        .bits_per_sample = ES7210_BITS_PER_SAMPLE,
    };
    if (esp_codec_dev_open(es7210_audio.record, &sample) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto unlock_fail;
    }
    const float gain = es7210_audio.gain_valid ? es7210_audio.gain_db : ES7210_DEFAULT_GAIN_DB;
    if (esp_codec_dev_set_in_gain(es7210_audio.record, gain) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto unlock_fail;
    }
    i2c_bus_unlock();
    es7210_audio.gain_db = gain;
    es7210_audio.gain_valid = true;
    es7210_audio.initialized = true;
    return ESP_OK;

unlock_fail:
    i2c_bus_unlock();
fail:
    es7210_deinit();
    return err;
}

static esp_err_t es7210_stream_open(void *user, const char *owner,
                                    const solar_os_stream_open_options_t *options,
                                    solar_os_stream_handle_t *handle)
{
    (void)owner;
    solar_os_es7210_device_t *device = user;
    if (device == NULL || !device->attached || handle == NULL ||
        !es7210_requested_format_supported(options != NULL ? &options->requested_audio : NULL)) {
        return device != NULL && device->attached ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = es7210_init();
    if (err == ESP_OK) {
        handle->context = device;
        handle->audio = es7210_native_format;
    }
    return err;
}

static void es7210_stream_close(void *user, solar_os_stream_handle_t *handle)
{
    (void)user;
    es7210_deinit();
    handle->context = NULL;
}

static esp_err_t es7210_stream_read(void *user, solar_os_stream_handle_t *handle,
                                    void *data, size_t len, uint32_t timeout_ms,
                                    size_t *read_len)
{
    (void)timeout_ms;
    solar_os_es7210_device_t *device = user;
    if (device == NULL || handle == NULL || handle->context != device ||
        data == NULL || read_len == NULL || len == 0U || len > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len % (ES7210_CHANNELS * sizeof(int16_t)) != 0U) return ESP_ERR_INVALID_SIZE;
    const int result = esp_codec_dev_read(device->record, data, (int)len);
    *read_len = result == ESP_CODEC_DEV_OK ? len : 0U;
    return result == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t es7210_set_input_gain(void *user, float gain_db)
{
    solar_os_es7210_device_t *device = user;
    if (device == NULL || !device->attached) return ESP_ERR_INVALID_STATE;
    if (!device->initialized) {
        device->gain_db = gain_db;
        device->gain_valid = true;
        return ESP_OK;
    }
    i2c_bus_lock();
    const int result = esp_codec_dev_set_in_gain(device->record, gain_db);
    i2c_bus_unlock();
    if (result != ESP_CODEC_DEV_OK) return ESP_FAIL;
    device->gain_db = gain_db;
    device->gain_valid = true;
    return ESP_OK;
}

static esp_err_t es7210_get_input_gain(void *user, float *gain_db)
{
    solar_os_es7210_device_t *device = user;
    if (device == NULL || gain_db == NULL || !device->attached) return ESP_ERR_INVALID_ARG;
    *gain_db = device->gain_valid ? device->gain_db : ES7210_DEFAULT_GAIN_DB;
    return ESP_OK;
}

static esp_err_t es7210_register_stream(solar_os_es7210_device_t *device)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_AUDIO,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
            .audio = es7210_native_format,
        },
        .open = es7210_stream_open,
        .close = es7210_stream_close,
        .read = es7210_stream_read,
        .user = device,
    };
    strlcpy(driver.info.id, device->stream_id, sizeof(driver.info.id));
    strlcpy(driver.info.provider, "es7210", sizeof(driver.info.provider));
    strlcpy(driver.info.device, device->id, sizeof(driver.info.device));
    strlcpy(driver.info.unit, "frames", sizeof(driver.info.unit));
    strlcpy(driver.info.format, "pcm-s16le", sizeof(driver.info.format));
    strlcpy(driver.info.summary, "ES7210 microphone capture", sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

esp_err_t solar_os_es7210_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || bindings == NULL || binding_count == 0U ||
        strnlen(name, ES7210_DEVICE_NAME_MAX) >= ES7210_DEVICE_NAME_MAX ||
        es7210_audio.attached) {
        return es7210_audio.attached ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_ARG;
    }
    const char *i2c_bus = NULL;
    int i2s_port = -1, mclk_pin = -1, bck_pin = -1, ws_pin = -1, din_pin = -1;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) i2c_bus = binding->target;
        else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) i2s_port = binding->value;
        else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (strcmp(binding->role, "mclk") == 0) mclk_pin = binding->value;
            else if (strcmp(binding->role, "bck") == 0) bck_pin = binding->value;
            else if (strcmp(binding->role, "ws") == 0) ws_pin = binding->value;
            else if (strcmp(binding->role, "din") == 0) din_pin = binding->value;
        }
    }
    if (i2c_bus == NULL || i2s_port < 0 || i2s_port >= SOC_I2S_NUM ||
        !GPIO_IS_VALID_OUTPUT_GPIO(mclk_pin) || !GPIO_IS_VALID_OUTPUT_GPIO(bck_pin) ||
        !GPIO_IS_VALID_OUTPUT_GPIO(ws_pin) || !GPIO_IS_VALID_GPIO(din_pin) ||
        mclk_pin == bck_pin || mclk_pin == ws_pin || mclk_pin == din_pin ||
        bck_pin == ws_pin || bck_pin == din_pin || ws_pin == din_pin) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_handle_t i2c_handle = NULL;
    int i2c_port = -1;
    esp_err_t err = solar_os_bus_i2c_get_handle(i2c_bus, &i2c_handle, &i2c_port);
    if (err != ESP_OK) return err;

    memset(&es7210_audio, 0, sizeof(es7210_audio));
    es7210_audio.i2s_port = i2s_port;
    es7210_audio.i2c_port = i2c_port;
    es7210_audio.i2c_handle = i2c_handle;
    es7210_audio.mclk_pin = mclk_pin;
    es7210_audio.bck_pin = bck_pin;
    es7210_audio.ws_pin = ws_pin;
    es7210_audio.din_pin = din_pin;
    strlcpy(es7210_audio.id, name, sizeof(es7210_audio.id));
    const int stream_len = snprintf(es7210_audio.stream_id, sizeof(es7210_audio.stream_id),
                                    "%s.capture", name);
    if (stream_len < 0 || (size_t)stream_len >= sizeof(es7210_audio.stream_id)) {
        memset(&es7210_audio, 0, sizeof(es7210_audio));
        return ESP_ERR_INVALID_ARG;
    }
    es7210_audio.attached = true;
    err = es7210_register_stream(&es7210_audio);
    if (err != ESP_OK) goto fail;

    solar_os_audio_device_info_t info = {
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT |
                        SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN,
        .native_format = es7210_native_format,
        .input_gain_min_db = 0.0f,
        .input_gain_max_db = 37.5f,
        .input_gain_step_db = 3.0f,
    };
    strlcpy(info.id, es7210_audio.id, sizeof(info.id));
    snprintf(info.name, sizeof(info.name), "ES7210 I2S%d microphone", i2s_port);
    strlcpy(info.provider, "expansion", sizeof(info.provider));
    strlcpy(info.capture_stream, es7210_audio.stream_id, sizeof(info.capture_stream));
    const solar_os_audio_device_ops_t ops = {
        .set_input_gain = es7210_set_input_gain,
        .get_input_gain = es7210_get_input_gain,
    };
    err = solar_os_audio_register_device_ex(&info, &ops, &es7210_audio);
    if (err == ESP_OK) return ESP_OK;
    (void)solar_os_stream_unregister(es7210_audio.stream_id);
fail:
    memset(&es7210_audio, 0, sizeof(es7210_audio));
    return err;
}

esp_err_t solar_os_es7210_detach(const char *name)
{
    if (name == NULL || !es7210_audio.attached || strcmp(name, es7210_audio.id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = solar_os_stream_unregister(es7210_audio.stream_id);
    if (err != ESP_OK) return err;
    err = solar_os_audio_unregister_device(es7210_audio.id);
    if (err != ESP_OK) return err;
    es7210_deinit();
    memset(&es7210_audio, 0, sizeof(es7210_audio));
    return ESP_OK;
}
