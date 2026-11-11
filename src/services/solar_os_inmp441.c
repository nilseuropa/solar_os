#include "solar_os_inmp441.h"

#include "inmp441.h"
#include "solar_os_audio.h"
#include "solar_os_stream.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    bool attached;
    uint8_t volume;
    const char *model, *provider;
    char id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    char capture[SOLAR_OS_STREAM_ID_MAX], playback[SOLAR_OS_STREAM_ID_MAX];
    solar_os_inmp441_t driver;
} microphone_t;
static microphone_t mic;

static solar_os_stream_audio_format_t format(unsigned channels) {
    return (solar_os_stream_audio_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = SOLAR_OS_INMP441_SAMPLE_RATE,
        .channels = channels,
        .bits_per_sample = 16,
        .frames_per_block = SOLAR_OS_INMP441_BLOCK_FRAMES};
}

static esp_err_t open_stream(void *user, const char *owner,
                             const solar_os_stream_open_options_t *options,
                             solar_os_stream_handle_t *h) {
    (void)owner;
    microphone_t *d = user;
    if (d == NULL || !d->attached || h == NULL)
        return ESP_ERR_INVALID_STATE;
    const bool capture = h->direction == SOLAR_OS_STREAM_DIRECTION_SOURCE;
    unsigned channels = capture ? 1U : 2U;
    if (options != NULL) {
        const solar_os_stream_audio_format_t *f = &options->requested_audio;
        if (f->sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
            (f->sample_rate != 0 &&
             f->sample_rate != SOLAR_OS_INMP441_SAMPLE_RATE) ||
            (f->bits_per_sample != 0 && f->bits_per_sample != 16) ||
            (f->channels != 0 && f->channels != 1 &&
             (capture || f->channels != 2))) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (f->channels != 0)
            channels = f->channels;
    }
    esp_err_t err = solar_os_inmp441_open(&d->driver, capture);
    if (err == ESP_OK) {
        h->context = d;
        h->audio = format(channels);
    }
    return err;
}

static void close_stream(void *user, solar_os_stream_handle_t *h) {
    microphone_t *d = user;
    solar_os_inmp441_close(&d->driver,
                           h->direction == SOLAR_OS_STREAM_DIRECTION_SOURCE);
    h->context = NULL;
}

static esp_err_t read_stream(void *user, solar_os_stream_handle_t *h,
                             void *data, size_t len, uint32_t timeout,
                             size_t *read) {
    microphone_t *d = user;
    if (h == NULL || h->context != d || data == NULL || read == NULL ||
        len == 0 || len % sizeof(int16_t) != 0)
        return ESP_ERR_INVALID_ARG;
    size_t frames = 0;
    esp_err_t err = solar_os_inmp441_read(
        &d->driver, data, len / sizeof(int16_t), timeout, &frames);
    *read = frames * sizeof(int16_t);
    return err;
}

static esp_err_t write_stream(void *user, solar_os_stream_handle_t *h,
                              const void *data, size_t len, uint32_t timeout,
                              size_t *written) {
    microphone_t *d = user;
    if (h == NULL || h->context != d || data == NULL || written == NULL ||
        h->audio.channels == 0 || len == 0 ||
        len % (sizeof(int16_t) * h->audio.channels) != 0)
        return ESP_ERR_INVALID_ARG;
    size_t frames = 0;
    esp_err_t err = solar_os_inmp441_write(
        &d->driver, data, len / (sizeof(int16_t) * h->audio.channels),
        h->audio.channels, d->volume, timeout, &frames);
    *written = frames * sizeof(int16_t) * h->audio.channels;
    return err;
}

static esp_err_t set_volume(void *user, uint8_t volume) {
    microphone_t *d = user;
    if (d == NULL || !d->attached || volume > 100)
        return ESP_ERR_INVALID_ARG;
    d->volume = volume;
    return ESP_OK;
}

static esp_err_t register_stream(bool capture) {
    solar_os_stream_driver_t stream = {
        .info = {.type = SOLAR_OS_STREAM_TYPE_AUDIO,
                 .direction = capture ? SOLAR_OS_STREAM_DIRECTION_SOURCE
                                      : SOLAR_OS_STREAM_DIRECTION_SINK,
                 .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
                 .audio = format(capture ? 1U : 2U)},
        .open = open_stream,
        .close = close_stream,
        .user = &mic,
        .read = capture ? read_stream : NULL,
        .write = capture ? NULL : write_stream,
    };
    strlcpy(stream.info.id, capture ? mic.capture : mic.playback,
            sizeof(stream.info.id));
    strlcpy(stream.info.device, mic.id, sizeof(stream.info.device));
    strlcpy(stream.info.provider, mic.provider, sizeof(stream.info.provider));
    strlcpy(stream.info.unit, "frames", sizeof(stream.info.unit));
    strlcpy(stream.info.format, "pcm-s16le", sizeof(stream.info.format));
    if (capture)
        snprintf(stream.info.summary, sizeof(stream.info.summary),
                 "%s mono capture", mic.model);
    else
        strlcpy(stream.info.summary, "shared I2S stereo playback",
                sizeof(stream.info.summary));
    return solar_os_stream_register(&stream);
}

static esp_err_t attach(const char *name,
                        const solar_os_expansion_binding_t *bindings,
                        size_t count, const char *model,
                        const char *provider) {
    if (mic.attached)
        return ESP_ERR_INVALID_STATE;
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof(mic.id) ||
        bindings == NULL)
        return ESP_ERR_INVALID_ARG;
    solar_os_inmp441_config_t c = {
        .port = -1, .bck = -1, .ws = -1, .din = -1, .dout = -1, .power = -1};
    for (size_t i = 0; i < count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT)
            c.port = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (strcmp(b->role, "bck") == 0)
                c.bck = b->value;
            else if (strcmp(b->role, "ws") == 0)
                c.ws = b->value;
            else if (strcmp(b->role, "din") == 0)
                c.din = b->value;
            else if (strcmp(b->role, "dout") == 0)
                c.dout = b->value;
            else if (strcmp(b->role, "power") == 0)
                c.power = b->value;
            else
                return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(b->role, "slot") == 0)
            c.slot = (unsigned)b->value;
        else
            return ESP_ERR_INVALID_ARG;
    }
    memset(&mic, 0, sizeof(mic));
    esp_err_t err = solar_os_inmp441_init(&mic.driver, &c);
    if (err != ESP_OK)
        return err;
    mic.attached = true;
    mic.volume = 50;
    mic.model = model;
    mic.provider = provider;
    strlcpy(mic.id, name, sizeof(mic.id));
    snprintf(mic.capture, sizeof(mic.capture), "%s.capture", name);
    snprintf(mic.playback, sizeof(mic.playback), "%s.playback", name);
    err = register_stream(true);
    bool source_registered = err == ESP_OK, sink_registered = false;
    if (err == ESP_OK && c.dout >= 0) {
        err = register_stream(false);
        sink_registered = err == ESP_OK;
    }
    if (err == ESP_OK) {
        solar_os_audio_device_info_t info = {
            .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_INPUT,
            .native_format = format(1)};
        if (c.dout >= 0)
            info.capabilities |= SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT |
                                 SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME;
        strlcpy(info.id, name, sizeof(info.id));
        snprintf(info.name, sizeof(info.name), "%s %s", model,
                 c.dout >= 0 ? "mic + I2S DAC/amp" : "microphone");
        strlcpy(info.provider, "expansion", sizeof(info.provider));
        strlcpy(info.capture_stream, mic.capture, sizeof(info.capture_stream));
        if (c.dout >= 0)
            strlcpy(info.playback_stream, mic.playback,
                    sizeof(info.playback_stream));
        const solar_os_audio_device_ops_t ops = {
            .set_volume = c.dout >= 0 ? set_volume : NULL};
        err = solar_os_audio_register_device_ex(&info, &ops, &mic);
    }
    if (err != ESP_OK) {
        if (sink_registered)
            (void)solar_os_stream_unregister(mic.playback);
        if (source_registered)
            (void)solar_os_stream_unregister(mic.capture);
        solar_os_inmp441_deinit(&mic.driver);
        memset(&mic, 0, sizeof(mic));
    }
    return err;
}

esp_err_t solar_os_inmp441_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t count) {
    return attach(name, bindings, count, "INMP441", "inmp441");
}

esp_err_t solar_os_ics43432_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t count) {
    return attach(name, bindings, count, "ICS-43432", "ics43432");
}

esp_err_t solar_os_inmp441_detach(const char *name) {
    if (!mic.attached || name == NULL || strcmp(name, mic.id) != 0)
        return ESP_ERR_NOT_FOUND;
    const bool duplex = mic.driver.config.dout >= 0;
    const char *ids[] = {mic.capture, mic.playback};
    esp_err_t err = solar_os_stream_unregister_many(ids, duplex ? 2U : 1U);
    if (err != ESP_OK)
        return err;
    err = solar_os_audio_unregister_device(name);
    if (err != ESP_OK) {
        (void)register_stream(true);
        if (duplex)
            (void)register_stream(false);
        return err;
    }
    solar_os_inmp441_deinit(&mic.driver);
    memset(&mic, 0, sizeof(mic));
    return ESP_OK;
}
