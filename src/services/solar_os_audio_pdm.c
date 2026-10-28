#include "solar_os_audio_pdm.h"

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "audio_pdm.h"
#include "solar_os_audio.h"
#include "solar_os_stream.h"

static struct {
    bool attached;
    int clk, data, power, slot;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char capture[SOLAR_OS_STREAM_ID_MAX];
} mic;

static const solar_os_stream_audio_format_t native_format = {
    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
    .sample_rate = AUDIO_PDM_SAMPLE_RATE,
    .channels = 1,
    .bits_per_sample = 16,
    .frames_per_block = AUDIO_PDM_FRAMES_PER_BLOCK,
};

static esp_err_t open_stream(void *user, const char *owner,
    const solar_os_stream_open_options_t *options, solar_os_stream_handle_t *handle) {
    (void)owner;
    if (user != &mic || !mic.attached || !handle) return ESP_ERR_INVALID_STATE;
    if (options) {
        const solar_os_stream_audio_format_t *format = &options->requested_audio;
        if (format->sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
            (format->sample_rate && format->sample_rate != AUDIO_PDM_SAMPLE_RATE) ||
            (format->channels && format->channels != 1) ||
            (format->bits_per_sample && format->bits_per_sample != 16)) return ESP_ERR_NOT_SUPPORTED;
    }
    const esp_err_t ret = audio_pdm_open(mic.clk, mic.data, mic.power, (unsigned)mic.slot);
    if (ret == ESP_OK) { handle->context = &mic; handle->audio = native_format; }
    return ret;
}

static void close_stream(void *user, solar_os_stream_handle_t *handle) {
    if (user == &mic && handle && handle->context == &mic) {
        (void)audio_pdm_close(); handle->context = NULL;
    }
}

static esp_err_t read_stream(void *user, solar_os_stream_handle_t *handle,
    void *data, size_t len, uint32_t timeout, size_t *read_len) {
    if (user != &mic || !handle || handle->context != &mic) return ESP_ERR_INVALID_STATE;
    return audio_pdm_read(data, len, timeout, read_len);
}

static esp_err_t register_stream(void) {
    solar_os_stream_driver_t stream = {
        .info = {.type=SOLAR_OS_STREAM_TYPE_AUDIO, .direction=SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE, .audio=native_format},
        .open=open_stream, .close=close_stream, .read=read_stream, .user=&mic,
    };
    strlcpy(stream.info.id, mic.capture, sizeof(stream.info.id));
    strlcpy(stream.info.device, mic.id, sizeof(stream.info.device));
    strlcpy(stream.info.provider, "audio-pdm", sizeof(stream.info.provider));
    strlcpy(stream.info.unit, "frames", sizeof(stream.info.unit));
    strlcpy(stream.info.format, "pcm-s16le", sizeof(stream.info.format));
    strlcpy(stream.info.summary, "PDM mono microphone capture", sizeof(stream.info.summary));
    return solar_os_stream_register(&stream);
}

esp_err_t solar_os_audio_pdm_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count) {
    if (mic.attached) return ESP_ERR_INVALID_STATE;
    if (!name || !name[0] || strlen(name) >= sizeof(mic.id) || !bindings) return ESP_ERR_INVALID_ARG;
    int port = -1, clk = -1, data = -1, power = -1, slot = 0;
    unsigned seen = 0;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        int *destination = NULL;
        unsigned bit = 0;
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT) {
            destination = &port; bit = 1;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (!strcmp(binding->role, "clk")) { destination = &clk; bit = 2; }
            else if (!strcmp(binding->role, "din")) { destination = &data; bit = 4; }
            else if (!strcmp(binding->role, "power")) { destination = &power; bit = 8; }
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER && !strcmp(binding->role, "slot")) {
            destination = &slot; bit = 16;
        }
        if (!destination || (seen & bit)) return ESP_ERR_INVALID_ARG;
        *destination = binding->value; seen |= bit;
    }
    if (port != 0 || !GPIO_IS_VALID_OUTPUT_GPIO(clk) || !GPIO_IS_VALID_GPIO(data) ||
        (power != -1 && !GPIO_IS_VALID_OUTPUT_GPIO(power)) || slot < 0 || slot > 1 ||
        clk == data || clk == power || data == power) return ESP_ERR_INVALID_ARG;
    esp_err_t ret = audio_pdm_prepare_power(power);
    if (ret != ESP_OK) return ret;
    memset(&mic, 0, sizeof(mic));
    mic.clk = clk; mic.data = data; mic.power = power; mic.slot = slot;
    strlcpy(mic.id, name, sizeof(mic.id));
    const int length = snprintf(mic.capture, sizeof(mic.capture), "%s.capture", name);
    if (length < 0 || (size_t)length >= sizeof(mic.capture)) return ESP_ERR_INVALID_SIZE;
    ret = register_stream();
    if (ret == ESP_OK) {
        solar_os_audio_device_info_t info = {
            .capabilities=SOLAR_OS_AUDIO_DEVICE_CAP_INPUT, .native_format=native_format,
        };
        strlcpy(info.id, mic.id, sizeof(info.id));
        strlcpy(info.name, "PDM microphone", sizeof(info.name));
        strlcpy(info.provider, "expansion", sizeof(info.provider));
        strlcpy(info.capture_stream, mic.capture, sizeof(info.capture_stream));
        ret = solar_os_audio_register_device(&info);
        if (ret != ESP_OK) (void)solar_os_stream_unregister(mic.capture);
    }
    if (ret == ESP_OK) mic.attached = true;
    else memset(&mic, 0, sizeof(mic));
    return ret;
}

esp_err_t solar_os_audio_pdm_detach(const char *name) {
    if (!name || !mic.attached || strcmp(name, mic.id)) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = solar_os_stream_unregister(mic.capture);
    if (ret != ESP_OK) return ret;
    ret = audio_pdm_close();
    if (ret == ESP_OK) ret = solar_os_audio_unregister_device(mic.id);
    if (ret != ESP_OK) { (void)register_stream(); return ret; }
    memset(&mic, 0, sizeof(mic));
    return ESP_OK;
}
