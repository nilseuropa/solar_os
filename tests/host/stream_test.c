#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_stream.h"

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

uint64_t solar_os_time_uptime_ms(void)
{
    return 1234U;
}

esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *epoch_ms)
{
    *epoch_ms = 5678U;
    return ESP_OK;
}

static esp_err_t read_scalar(void *user,
                             const solar_os_stream_read_options_t *options,
                             float *value)
{
    (void)options;
    *value = *(const float *)user;
    return ESP_OK;
}

static esp_err_t read_sample(void *user,
    const solar_os_stream_read_options_t *options, solar_os_stream_scalar_sample_t *sample)
{
    esp_err_t err = read_scalar(user, options, &sample->value);
    sample->uptime_ms = 1000; /* Cached acquisition time must survive the read. */
    return err;
}

static esp_err_t write_audio(void *user,
                             solar_os_stream_handle_t *handle,
                             const void *data,
                             size_t len,
                             uint32_t timeout_ms,
                             size_t *written)
{
    (void)handle;
    (void)data;
    (void)timeout_ms;
    *(size_t *)user += len;
    *written = len;
    return ESP_OK;
}

static esp_err_t open_audio(void *user,
                            const char *owner,
                            const solar_os_stream_open_options_t *options,
                            solar_os_stream_handle_t *handle)
{
    (void)user;
    (void)owner;
    if (options->requested_audio.channels != 0U) {
        handle->audio.channels = options->requested_audio.channels;
    }
    return ESP_OK;
}

static solar_os_stream_driver_t scalar_driver(float *value)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_SCALAR,
            .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
            .sharing = SOLAR_OS_STREAM_SHARING_SHARED,
        },
        .read_scalar = read_scalar,
        .user = value,
    };
    strcpy(driver.info.id, "sensor0");
    strcpy(driver.info.provider, "test");
    strcpy(driver.info.device, "device0");
    strcpy(driver.info.unit, "C");
    strcpy(driver.info.format, "f32");
    strcpy(driver.info.summary, "test sensor");
    return driver;
}

static solar_os_stream_driver_t audio_driver(size_t *bytes)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = SOLAR_OS_STREAM_TYPE_AUDIO,
            .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
            .sharing = SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
            .audio = {
                .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                .sample_rate = 16000U,
                .channels = 2U,
                .bits_per_sample = 16U,
                .frames_per_block = 4U,
            },
        },
        .open = open_audio,
        .write = write_audio,
        .user = bytes,
    };
    strcpy(driver.info.id, "audio0.playback");
    strcpy(driver.info.provider, "test");
    strcpy(driver.info.device, "audio0");
    strcpy(driver.info.unit, "frames");
    strcpy(driver.info.format, "pcm-s16le");
    return driver;
}

int main(void)
{
    assert(solar_os_stream_init() == ESP_OK);
    float scalar_value = 21.5f;
    solar_os_stream_driver_t scalar = scalar_driver(&scalar_value);
    assert(solar_os_stream_register(&scalar) == ESP_OK);
    assert(solar_os_stream_register(&scalar) == ESP_ERR_INVALID_STATE);

    solar_os_stream_handle_t first = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_handle_t second = SOLAR_OS_STREAM_HANDLE_INIT;
    assert(solar_os_stream_open("sensor0", "first", &first) == ESP_OK);
    assert(solar_os_stream_open("sensor0", "second", &second) == ESP_OK);
    float value = 0.0f;
    assert(solar_os_stream_read_scalar(&first, NULL, &value) == ESP_OK);
    assert(value == scalar_value);
    solar_os_stream_scalar_sample_t sample;
    assert(solar_os_stream_read_scalar_sample(&first, NULL, &sample) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_stream_unregister("sensor0") == ESP_ERR_INVALID_STATE);

    solar_os_stream_info_t info;
    assert(solar_os_stream_get_info("sensor0", &info) == ESP_OK);
    assert(info.active_handles == 2U);
    assert(strcmp(info.owner, "shared") == 0);
    assert(info.read_units == 1U);
    solar_os_stream_close(&first);
    solar_os_stream_close(&second);
    assert(solar_os_stream_unregister("sensor0") == ESP_OK);
    assert(!solar_os_stream_handle_valid(&first));
    scalar.read_scalar_sample = read_sample;
    assert(solar_os_stream_register(&scalar) == ESP_OK);
    assert(solar_os_stream_open("sensor0", "feedback", &first) == ESP_OK);
    assert(solar_os_stream_read_scalar_sample(&first, NULL, &sample) == ESP_OK);
    assert(sample.value == scalar_value && sample.uptime_ms == 1000);
    solar_os_stream_close(&first);
    assert(solar_os_stream_read_scalar_sample(&first, NULL, &sample) != ESP_OK);
    assert(solar_os_stream_unregister("sensor0") == ESP_OK);

    size_t bytes = 0U;
    solar_os_stream_driver_t audio = audio_driver(&bytes);
    assert(solar_os_stream_register(&audio) == ESP_OK);
    solar_os_stream_open_options_t source_options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
    };
    assert(solar_os_stream_open_ex("audio0.playback", "reader",
                                   &source_options, &first) ==
           ESP_ERR_NOT_SUPPORTED);

    solar_os_stream_open_options_t sink_options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
        .requested_audio.channels = 1U,
    };
    assert(solar_os_stream_open_ex("audio0.playback", "synth",
                                   &sink_options, &first) == ESP_OK);
    assert(solar_os_stream_open("audio0.playback", "aplay", &second) ==
           ESP_ERR_INVALID_STATE);
    int16_t samples[8] = {0};
    size_t written = 0U;
    assert(solar_os_stream_write(&first, samples, sizeof(samples), 0U,
                                 &written) == ESP_OK);
    assert(written == sizeof(samples));
    assert(bytes == sizeof(samples));
    assert(solar_os_stream_get_info("audio0.playback", &info) == ESP_OK);
    assert(info.written_units == 8U);
    assert(strcmp(info.owner, "synth") == 0);
    assert(solar_os_stream_register(&scalar) == ESP_OK);
    const char *related[] = {"sensor0", "audio0.playback"};
    assert(solar_os_stream_unregister_many(related, 2U) == ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_get_info("sensor0", &info) == ESP_OK);
    const char *missing[] = {"sensor0", "missing"};
    assert(solar_os_stream_unregister_many(missing, 2U) == ESP_ERR_NOT_FOUND);
    assert(solar_os_stream_get_info("sensor0", &info) == ESP_OK);
    solar_os_stream_close(&first);
    assert(solar_os_stream_unregister_many(related, 2U) == ESP_OK);
    assert(solar_os_stream_get_info("sensor0", &info) == ESP_ERR_NOT_FOUND);
    assert(solar_os_stream_get_info("audio0.playback", &info) == ESP_ERR_NOT_FOUND);

    /* Timestamp-only scalar sources also support ordinary scalar readers. */
    scalar.read_scalar=NULL;
    assert(solar_os_stream_register(&scalar)==ESP_OK);
    assert(solar_os_stream_open("sensor0","plain-reader",&first)==ESP_OK);
    assert(solar_os_stream_read_scalar(&first,NULL,&value)==ESP_OK && value==scalar_value);
    assert(solar_os_stream_close_ex(&first)==ESP_OK);
    assert(solar_os_stream_unregister("sensor0")==ESP_OK);
    solar_os_stream_driver_t sink={.info={.id="actuator0",.provider="test",.format="f32",
        .type=SOLAR_OS_STREAM_TYPE_SCALAR,.direction=SOLAR_OS_STREAM_DIRECTION_SINK,
        .sharing=SOLAR_OS_STREAM_SHARING_EXCLUSIVE},.write=write_audio,.user=&bytes};
    assert(solar_os_stream_register(&sink)==ESP_OK);
    assert(solar_os_stream_open("actuator0","controller",&first)==ESP_OK);
    assert(solar_os_stream_read_scalar(&first,NULL,&value)==ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_stream_write(&first,&scalar_value,sizeof(scalar_value),0,&written)==ESP_OK && written==sizeof(scalar_value));
    assert(solar_os_stream_open("actuator0","competitor",&second)==ESP_ERR_INVALID_STATE);
    assert(solar_os_stream_close_ex(&first)==ESP_OK);
    assert(solar_os_stream_unregister("actuator0")==ESP_OK);
    puts("stream registry tests: ok");
    return 0;
}
