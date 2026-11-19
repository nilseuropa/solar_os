#include "solar_os_es7243.h"

#include <limits.h>
#include "es7243_gain.h"
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "i2c_bus.h"
#include "soc/soc_caps.h"
#include "freertos/semphr.h"
#include "solar_os_audio.h"
#include "solar_os_buses.h"
#include "solar_os_stream.h"

#define ES7243_DEVICE_NAME_MAX 16U
#define ES7243_SAMPLE_RATE 16000U
#define ES7243_CHANNELS 2U
#define ES7243_BITS_PER_SAMPLE 16U
#define ES7243_FRAMES_PER_BLOCK 128U
#define ES7243_DMA_DESC_NUM 4U
#define ES7243_DEFAULT_GAIN_DB 27.0f

typedef struct {
    bool attached;
    bool variant_e;
    uint8_t address;
    bool initialized;
    bool rx_enabled;
    bool gain_valid;
    bool stream_registered, audio_registered;
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
    float requested_gain_db;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char stream_id[SOLAR_OS_STREAM_ID_MAX];
} solar_os_es7243_device_t;

static solar_os_es7243_device_t es7243_audio;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;

static const solar_os_stream_audio_format_t es7243_native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = ES7243_SAMPLE_RATE,
    .channels = ES7243_CHANNELS,
    .bits_per_sample = ES7243_BITS_PER_SAMPLE,
    .frames_per_block = ES7243_FRAMES_PER_BLOCK,
};

static bool es7243_requested_format_supported(
    const solar_os_stream_audio_format_t *format)
{
    return format == NULL ||
        ((format->sample_rate == 0U || format->sample_rate == ES7243_SAMPLE_RATE) &&
         (format->channels == 0U || format->channels == ES7243_CHANNELS) &&
         (format->bits_per_sample == 0U || format->bits_per_sample == ES7243_BITS_PER_SAMPLE) &&
         format->sample_format == SOLAR_OS_STREAM_AUDIO_S16_LE);
}

static void es7243_deinit(void)
{
    if (es7243_audio.record != NULL) {
        (void)esp_codec_dev_close(es7243_audio.record);
        esp_codec_dev_delete(es7243_audio.record);
    }
    if (es7243_audio.codec_if != NULL) audio_codec_delete_codec_if(es7243_audio.codec_if);
    if (es7243_audio.ctrl_if != NULL) audio_codec_delete_ctrl_if(es7243_audio.ctrl_if);
    if (es7243_audio.data_if != NULL) audio_codec_delete_data_if(es7243_audio.data_if);
    if (es7243_audio.rx_enabled) (void)i2s_channel_disable(es7243_audio.rx_handle);
    if (es7243_audio.rx_handle != NULL) (void)i2s_del_channel(es7243_audio.rx_handle);

    es7243_audio.initialized = false;
    es7243_audio.rx_enabled = false;
    es7243_audio.rx_handle = NULL;
    es7243_audio.data_if = NULL;
    es7243_audio.ctrl_if = NULL;
    es7243_audio.codec_if = NULL;
    es7243_audio.record = NULL;
}

static esp_err_t es7243_init(void)
{
    if (es7243_audio.initialized) return ESP_OK;
    if (!es7243_audio.attached || es7243_audio.i2c_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(
        es7243_audio.i2s_port, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = ES7243_DMA_DESC_NUM;
    channel_config.dma_frame_num = ES7243_FRAMES_PER_BLOCK;
    esp_err_t err = i2s_new_channel(&channel_config, NULL, &es7243_audio.rx_handle);
    if (err != ESP_OK) goto fail;

    i2s_std_config_t std_config = {
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            16, I2S_SLOT_MODE_STEREO),
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(ES7243_SAMPLE_RATE),
        .gpio_cfg = {
            .mclk = es7243_audio.mclk_pin,
            .bclk = es7243_audio.bck_pin,
            .ws = es7243_audio.ws_pin,
            .dout = I2S_GPIO_UNUSED,
            .din = es7243_audio.din_pin,
        },
    };
    std_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    err = i2s_channel_init_std_mode(es7243_audio.rx_handle, &std_config);
    if (err != ESP_OK) goto fail;
    err = i2s_channel_enable(es7243_audio.rx_handle);
    if (err != ESP_OK) goto fail;
    es7243_audio.rx_enabled = true;

    audio_codec_i2s_cfg_t data_config = {
        .port = es7243_audio.i2s_port,
        .rx_handle = es7243_audio.rx_handle,
    };
    es7243_audio.data_if = audio_codec_new_i2s_data(&data_config);
    if (es7243_audio.data_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }

    i2c_bus_lock();
    audio_codec_i2c_cfg_t i2c_config = {
        .port = es7243_audio.i2c_port,
        .addr = es7243_audio.address << 1,
        .bus_handle = es7243_audio.i2c_handle,
    };
    es7243_audio.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_config);
    if (es7243_audio.ctrl_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    if (es7243_audio.variant_e) {
        es7243e_codec_cfg_t codec_config = {.ctrl_if = es7243_audio.ctrl_if};
        es7243_audio.codec_if = es7243e_codec_new(&codec_config);
    } else {
        es7243_codec_cfg_t codec_config = {.ctrl_if = es7243_audio.ctrl_if};
        es7243_audio.codec_if = es7243_codec_new(&codec_config);
    }
    if (es7243_audio.codec_if == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    esp_codec_dev_cfg_t device_config = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = es7243_audio.codec_if,
        .data_if = es7243_audio.data_if,
    };
    es7243_audio.record = esp_codec_dev_new(&device_config);
    if (es7243_audio.record == NULL) {
        err = ESP_ERR_NO_MEM;
        goto unlock_fail;
    }
    esp_codec_dev_sample_info_t sample = {
        .sample_rate = ES7243_SAMPLE_RATE,
        .channel = ES7243_CHANNELS,
        .bits_per_sample = ES7243_BITS_PER_SAMPLE,
    };
    if (esp_codec_dev_open(es7243_audio.record, &sample) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto unlock_fail;
    }
    const float requested = es7243_audio.gain_valid ? es7243_audio.requested_gain_db : ES7243_DEFAULT_GAIN_DB;
    float gain;
    err = solar_os_es7243_gain(es7243_audio.variant_e, requested, &gain);
    if (err != ESP_OK) goto unlock_fail;
    if (esp_codec_dev_set_in_gain(es7243_audio.record, requested) != ESP_CODEC_DEV_OK) {
        err = ESP_FAIL;
        goto unlock_fail;
    }
    i2c_bus_unlock();
    es7243_audio.gain_db = gain;
    es7243_audio.requested_gain_db = requested;
    es7243_audio.gain_valid = true;
    es7243_audio.initialized = true;
    return ESP_OK;

unlock_fail:
    i2c_bus_unlock();
fail:
    es7243_deinit();
    return err;
}

static esp_err_t es7243_stream_open(void *user, const char *owner,
                                    const solar_os_stream_open_options_t *options,
                                    solar_os_stream_handle_t *handle)
{
    (void)owner;
    solar_os_es7243_device_t *device = user;
    if (device == NULL || !device->attached || handle == NULL ||
        !es7243_requested_format_supported(options != NULL ? &options->requested_audio : NULL)) {
        return device != NULL && device->attached ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const esp_err_t err = es7243_init();
    if (err == ESP_OK) {
        handle->context = device;
        handle->audio = es7243_native_format;
    }
    xSemaphoreGive(mutex);
    return err;
}

static void es7243_stream_close(void *user, solar_os_stream_handle_t *handle)
{
    (void)user;
    xSemaphoreTake(mutex, portMAX_DELAY);
    es7243_deinit();
    handle->context = NULL;
    xSemaphoreGive(mutex);
}

static esp_err_t es7243_stream_read(void *user, solar_os_stream_handle_t *handle,
                                    void *data, size_t len, uint32_t timeout_ms,
                                    size_t *read_len)
{
    solar_os_es7243_device_t *device = user;
    if (device == NULL || handle == NULL || handle->context != device ||
        data == NULL || read_len == NULL || len == 0U || len > INT_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    *read_len = 0;
    if (len % (ES7243_CHANNELS * sizeof(int16_t)) != 0U) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t error = i2s_channel_read(device->rx_handle, data, len, read_len, timeout_ms);
    xSemaphoreGive(mutex);
    return error;
}

static esp_err_t es7243_set_input_gain(void *user, float gain_db)
{
    solar_os_es7243_device_t *device = user;
    if (device == NULL || !device->attached) return ESP_ERR_INVALID_STATE;
    float applied;
    esp_err_t error = solar_os_es7243_gain(device->variant_e, gain_db, &applied);
    if (error) return error;
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (!device->initialized) {
        device->gain_db = applied;
        device->requested_gain_db = gain_db;
        device->gain_valid = true;
        xSemaphoreGive(mutex);
        return ESP_OK;
    }
    i2c_bus_lock();
    const int result = esp_codec_dev_set_in_gain(device->record, gain_db);
    i2c_bus_unlock();
    if (result != ESP_CODEC_DEV_OK) { xSemaphoreGive(mutex); return ESP_FAIL; }
    device->gain_db = applied;
    device->requested_gain_db = gain_db;
    device->gain_valid = true;
    xSemaphoreGive(mutex);
    return ESP_OK;
}

static esp_err_t es7243_get_input_gain(void *user, float *gain_db)
{
    solar_os_es7243_device_t *device = user;
    if (device == NULL || gain_db == NULL || !device->attached) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(mutex, portMAX_DELAY);
    *gain_db = device->gain_valid ? device->gain_db : ES7243_DEFAULT_GAIN_DB;
    xSemaphoreGive(mutex);
    return ESP_OK;
}

static esp_err_t es7243_register_stream(solar_os_es7243_device_t *device)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_AUDIO,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
            .audio = es7243_native_format,
        },
        .open = es7243_stream_open,
        .close = es7243_stream_close,
        .read = es7243_stream_read,
        .user = device,
    };
    strlcpy(driver.info.id, device->stream_id, sizeof(driver.info.id));
    strlcpy(driver.info.provider, device->variant_e ? "es7243e" : "es7243", sizeof(driver.info.provider));
    strlcpy(driver.info.device, device->id, sizeof(driver.info.device));
    strlcpy(driver.info.unit, "frames", sizeof(driver.info.unit));
    strlcpy(driver.info.format, "pcm-s16le", sizeof(driver.info.format));
    strlcpy(driver.info.summary, device->variant_e ? "ES7243E microphone capture" : "ES7243 microphone capture", sizeof(driver.info.summary));
    return solar_os_stream_register(&driver);
}

static esp_err_t attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count, bool variant_e)
{
    if (name == NULL || bindings == NULL || binding_count == 0U ||
        name[0] == '\0' || strnlen(name, ES7243_DEVICE_NAME_MAX) >= ES7243_DEVICE_NAME_MAX ||
        es7243_audio.attached) {
        return es7243_audio.attached ? ESP_ERR_NOT_ALLOWED : ESP_ERR_INVALID_ARG;
    }
    const char *i2c_bus = NULL;
    const char *address_bus = NULL;
    int address = -1;
    int i2s_port = -1, mclk_pin = -1, bck_pin = -1, ws_pin = -1, din_pin = -1;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        for (size_t j = 0; j < i; ++j)
            if (binding->kind == bindings[j].kind && !strcmp(binding->role, bindings[j].role))
                return ESP_ERR_INVALID_ARG;
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS) i2c_bus = binding->target;
        else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS) { address = binding->value; address_bus = binding->target; }
        else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) i2s_port = binding->value;
        else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (strcmp(binding->role, "mclk") == 0) mclk_pin = binding->value;
            else if (strcmp(binding->role, "bck") == 0) bck_pin = binding->value;
            else if (strcmp(binding->role, "ws") == 0) ws_pin = binding->value;
            else if (strcmp(binding->role, "din") == 0) din_pin = binding->value;
            else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (i2c_bus == NULL || !address_bus || (address_bus[0] && strcmp(i2c_bus, address_bus)) ||
        address < 0x08 || address > 0x77 || i2s_port < 0 || i2s_port >= SOC_I2S_NUM ||
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
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    if (!mutex) return ESP_ERR_NO_MEM;

    memset(&es7243_audio, 0, sizeof(es7243_audio));
    es7243_audio.variant_e = variant_e;
    es7243_audio.address = address;
    es7243_audio.i2s_port = i2s_port;
    es7243_audio.i2c_port = i2c_port;
    es7243_audio.i2c_handle = i2c_handle;
    es7243_audio.mclk_pin = mclk_pin;
    es7243_audio.bck_pin = bck_pin;
    es7243_audio.ws_pin = ws_pin;
    es7243_audio.din_pin = din_pin;
    strlcpy(es7243_audio.id, name, sizeof(es7243_audio.id));
    const int stream_len = snprintf(es7243_audio.stream_id, sizeof(es7243_audio.stream_id),
                                    "%s.capture", name);
    if (stream_len < 0 || (size_t)stream_len >= sizeof(es7243_audio.stream_id)) {
        vSemaphoreDelete(mutex); mutex = NULL;
        memset(&es7243_audio, 0, sizeof(es7243_audio));
        return ESP_ERR_INVALID_ARG;
    }
    es7243_audio.attached = true;
    err = es7243_register_stream(&es7243_audio);
    if (err != ESP_OK) goto fail;
    es7243_audio.stream_registered = true;

    solar_os_audio_device_info_t info = {
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT |
                        SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN,
        .native_format = es7243_native_format,
        .input_gain_min_db = 0.0f,
        .input_gain_max_db = variant_e ? 37.5f : 27.0f,
        .input_gain_step_db = variant_e ? 3.0f : 0.5f,
    };
    strlcpy(info.id, es7243_audio.id, sizeof(info.id));
    snprintf(info.name, sizeof(info.name), "%s I2S%d microphone", variant_e ? "ES7243E" : "ES7243", i2s_port);
    strlcpy(info.provider, "expansion", sizeof(info.provider));
    strlcpy(info.capture_stream, es7243_audio.stream_id, sizeof(info.capture_stream));
    const solar_os_audio_device_ops_t ops = {
        .set_input_gain = es7243_set_input_gain,
        .get_input_gain = es7243_get_input_gain,
    };
    err = solar_os_audio_register_device_ex(&info, &ops, &es7243_audio);
    if (err == ESP_OK) { es7243_audio.audio_registered = true; return ESP_OK; }
    (void)solar_os_stream_unregister(es7243_audio.stream_id);
fail:
    vSemaphoreDelete(mutex); mutex = NULL;
    memset(&es7243_audio, 0, sizeof(es7243_audio));
    return err;
}

esp_err_t solar_os_es7243_detach(const char *name)
{
    if (name == NULL || !es7243_audio.attached || strcmp(name, es7243_audio.id) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err;
    if (es7243_audio.stream_registered) {
        err = solar_os_stream_unregister(es7243_audio.stream_id);
        if (err != ESP_OK) return err;
        es7243_audio.stream_registered = false;
    }
    if (es7243_audio.audio_registered) {
        err = solar_os_audio_unregister_device(es7243_audio.id);
        if (err != ESP_OK) return err;
        es7243_audio.audio_registered = false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    es7243_deinit();
    xSemaphoreGive(mutex);
    vSemaphoreDelete(mutex); mutex = NULL;
    memset(&es7243_audio, 0, sizeof(es7243_audio));
    return ESP_OK;
}

esp_err_t solar_os_es7243_attach(const char *name, const solar_os_expansion_binding_t *b, size_t count)
{ return attach(name, b, count, false); }
esp_err_t solar_os_es7243e_attach(const char *name, const solar_os_expansion_binding_t *b, size_t count)
{ return attach(name, b, count, true); }
