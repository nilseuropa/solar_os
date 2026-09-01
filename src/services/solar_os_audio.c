#include "solar_os_audio.h"

#include <inttypes.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_audio_player.h"
#include "solar_os_audio_pcm.h"
#define SOLAR_OS_AUDIO_BACKEND_PACKAGE ( \
    SOLAR_OS_PACKAGE_DRIVER_AUDIO_ES8311_CODECS || \
    SOLAR_OS_PACKAGE_DRIVER_AUDIO_ESP32_DAC)

#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
#include "solar_os_audio_backend.h"
#endif
#include "solar_os_task.h"
#include "esp_timer.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
#include "solar_os_audio_codec.h"
#endif
#include "solar_os_storage.h"

#define AUDIO_FRAME_CHUNK 256U
#define AUDIO_LEVEL_BUFFER_SAMPLES 512U
#define AUDIO_LOOPBACK_BUFFER_BYTES 2048U
#define AUDIO_TONE_AMPLITUDE 12000
#define AUDIO_WAV_HEADER_BYTES 44U
#define AUDIO_WAV_BUFFER_BYTES 4096U
#define AUDIO_WAV_PCM_FORMAT 1U
#define AUDIO_TONE_WORKER_STACK 4096U
#define AUDIO_TONE_WORKER_PRIORITY (tskIDLE_PRIORITY + 1U)
#define AUDIO_TONE_CANCEL_POLL_MS 10U
#define AUDIO_GLOBAL_DEFAULT_VOLUME 50U
#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
#define SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS 2U
typedef solar_os_audio_backend_status_t solar_os_board_audio_status_t;
#define solar_os_board_audio_init solar_os_audio_backend_init
#define solar_os_board_audio_deinit solar_os_audio_backend_deinit
#define solar_os_board_audio_set_volume solar_os_audio_backend_set_volume
#define solar_os_board_audio_set_mic_gain solar_os_audio_backend_set_mic_gain
#define solar_os_board_audio_write solar_os_audio_backend_write
#define solar_os_board_audio_read solar_os_audio_backend_read
#define solar_os_board_audio_get_status solar_os_audio_backend_get_status
#endif
#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
#define AUDIO_MP3_INPUT_BUFFER_BYTES 16384U
#define AUDIO_MP3_PROBE_SCAN_BYTES 65536U
#define AUDIO_MP3_OUTPUT_SAMPLES_MAX (AUDIO_WAV_BUFFER_BYTES / sizeof(int16_t))
#endif

static const char *TAG = "solar_os_audio";

typedef struct {
    bool registered;
    solar_os_audio_device_info_t info;
    solar_os_audio_device_ops_t ops;
    void *user;
} audio_device_entry_t;

static audio_device_entry_t audio_devices[SOLAR_OS_AUDIO_DEVICE_MAX];
static portMUX_TYPE audio_devices_lock = portMUX_INITIALIZER_UNLOCKED;
static char audio_default_output[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
static uint8_t audio_global_volume = AUDIO_GLOBAL_DEFAULT_VOLUME;
static uint8_t audio_mute_restore_volume = AUDIO_GLOBAL_DEFAULT_VOLUME;

static bool audio_volume_arg_valid(uint8_t volume)
{
    return volume <= 100U || volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL;
}

#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
typedef struct {
    uint32_t id;
    size_t step_count;
    uint8_t volume;
    bool drop_if_busy;
    solar_os_audio_tone_step_t steps[SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS];
} audio_tone_queue_entry_t;

typedef struct {
    SemaphoreHandle_t mutex;
    TaskHandle_t task;
    audio_tone_queue_entry_t entries[SOLAR_OS_AUDIO_TONE_QUEUE_CAPACITY];
    size_t count;
    uint32_t next_id;
    uint32_t current_id;
    volatile bool cancel_current;
    bool playing;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t dropped;
    uint32_t failed;
} audio_tone_queue_t;

static audio_tone_queue_t audio_tones;
static SemaphoreHandle_t audio_operation_mutex;
static StaticSemaphore_t audio_operation_mutex_storage;
static StaticSemaphore_t audio_tone_mutex_storage;
static portMUX_TYPE audio_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

struct solar_os_audio_stream {
    TaskHandle_t task;
    bool active;
    char owner[SOLAR_OS_AUDIO_STREAM_OWNER_MAX];
};

static struct solar_os_audio_stream audio_output_stream;

struct solar_os_audio_input_stream {
    TaskHandle_t task;
    bool active;
    char owner[SOLAR_OS_AUDIO_STREAM_OWNER_MAX];
};

static struct solar_os_audio_input_stream audio_input_stream;

static void audio_board_deinit_if_unused(void)
{
    if (!audio_output_stream.active && !audio_input_stream.active) {
        solar_os_board_audio_deinit();
    }
}

static esp_err_t audio_play_tone_locked(uint32_t frequency_hz,
                                        uint32_t duration_ms,
                                        uint8_t volume,
                                        const volatile bool *cancelled);
static void audio_tone_cancel_all(void);
#endif

#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
static uint8_t audio_resolve_playback_volume(uint8_t volume)
{
    return volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL ? audio_global_volume : volume;
}

static uint8_t clamp_percent_u32(uint32_t value)
{
    return value > 100U ? 100U : (uint8_t)value;
}

static esp_err_t audio_ensure_mutexes(void)
{
    portENTER_CRITICAL(&audio_mutex_init_lock);
    if (audio_operation_mutex == NULL) {
        audio_operation_mutex = xSemaphoreCreateRecursiveMutexStatic(
            &audio_operation_mutex_storage);
    }
    if (audio_tones.mutex == NULL) {
        audio_tones.mutex = xSemaphoreCreateMutexStatic(&audio_tone_mutex_storage);
        audio_tones.next_id = 1U;
    }
    const bool ready = audio_operation_mutex != NULL && audio_tones.mutex != NULL;
    portEXIT_CRITICAL(&audio_mutex_init_lock);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t audio_operation_take(TickType_t wait)
{
    const esp_err_t err = audio_ensure_mutexes();
    if (err != ESP_OK) {
        return err;
    }
    return xSemaphoreTakeRecursive(audio_operation_mutex, wait) == pdTRUE ?
        ESP_OK : ESP_ERR_INVALID_STATE;
}

static void audio_operation_give(void)
{
    if (audio_operation_mutex != NULL) {
        (void)xSemaphoreGiveRecursive(audio_operation_mutex);
    }
}

static TickType_t audio_timeout_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == UINT32_MAX) {
        return portMAX_DELAY;
    }
    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

static void audio_tone_lock(void)
{
    (void)xSemaphoreTake(audio_tones.mutex, portMAX_DELAY);
}

static void audio_tone_unlock(void)
{
    (void)xSemaphoreGive(audio_tones.mutex);
}

static uint32_t audio_abs_i16(int16_t value)
{
    return value < 0 ? (uint32_t)(-(int32_t)value) : (uint32_t)value;
}
#endif

static uint16_t audio_get_u16le(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t audio_get_u32le(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static void audio_put_u16le(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)(value >> 8);
}

static void audio_put_u32le(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xffU);
    data[1] = (uint8_t)((value >> 8) & 0xffU);
    data[2] = (uint8_t)((value >> 16) & 0xffU);
    data[3] = (uint8_t)((value >> 24) & 0xffU);
}

static void audio_wav_build_header(uint8_t header[AUDIO_WAV_HEADER_BYTES],
                                   const solar_os_audio_wav_info_t *info)
{
    memset(header, 0, AUDIO_WAV_HEADER_BYTES);
    memcpy(&header[0], "RIFF", 4);
    audio_put_u32le(&header[4], info->data_bytes + 36U);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    audio_put_u32le(&header[16], 16U);
    audio_put_u16le(&header[20], AUDIO_WAV_PCM_FORMAT);
    audio_put_u16le(&header[22], info->channels);
    audio_put_u32le(&header[24], info->sample_rate);
    audio_put_u32le(&header[28],
                    info->sample_rate * info->channels * ((uint32_t)info->bits_per_sample / 8U));
    audio_put_u16le(&header[32], info->block_align);
    audio_put_u16le(&header[34], info->bits_per_sample);
    memcpy(&header[36], "data", 4);
    audio_put_u32le(&header[40], info->data_bytes);
}

static bool audio_wav_should_cancel(const solar_os_audio_wav_options_t *options)
{
    return options != NULL &&
        options->should_cancel != NULL &&
        options->should_cancel(options->user);
}

static bool audio_wav_should_pause(const solar_os_audio_wav_options_t *options)
{
    return options != NULL &&
        options->should_pause != NULL &&
        options->should_pause(options->user);
}

static bool audio_wav_should_monitor(
    const solar_os_audio_wav_options_t *options)
{
    if (options == NULL) {
        return false;
    }
    return options->should_monitor != NULL ?
        options->should_monitor(options->user) : options->monitor;
}

static uint32_t audio_wav_progress_interval_ms(const solar_os_audio_wav_options_t *options)
{
    if (options == NULL || options->progress_interval_ms == 0) {
        return SOLAR_OS_AUDIO_WAV_DEFAULT_PROGRESS_MS;
    }
    return options->progress_interval_ms;
}

static void audio_wav_report_progress(const solar_os_audio_wav_options_t *options,
                                      const solar_os_audio_wav_info_t *info,
                                      bool done,
                                      bool cancelled)
{
    if (options == NULL || options->progress == NULL || info == NULL) {
        return;
    }

    solar_os_audio_wav_progress_t progress = {
        .info = *info,
        .done = done,
        .cancelled = cancelled,
    };
    options->progress(&progress, options->user);
}

static esp_err_t audio_wav_write_exact(FILE *file, const void *data, size_t len)
{
    if (file == NULL || data == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return fwrite(data, 1, len, file) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_wav_read_exact(FILE *file, void *data, size_t len)
{
    if (file == NULL || data == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    return fread(data, 1, len, file) == len ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_wav_read_info_from_file(FILE *file,
                                               solar_os_audio_wav_info_t *info,
                                               long *data_offset)
{
    uint8_t header[12];
    bool have_fmt = false;
    bool have_data = false;
    long found_data_offset = 0;
    uint32_t found_data_bytes = 0;

    if (file == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    if (audio_wav_read_exact(file, header, sizeof(header)) != ESP_OK ||
        memcmp(&header[0], "RIFF", 4) != 0 ||
        memcmp(&header[8], "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    while (!feof(file)) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            break;
        }

        const uint32_t chunk_size = audio_get_u32le(&chunk[4]);
        const long chunk_data_offset = ftell(file);
        if (chunk_data_offset < 0) {
            return ESP_FAIL;
        }

        if (memcmp(&chunk[0], "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) ||
                audio_wav_read_exact(file, fmt, sizeof(fmt)) != ESP_OK) {
                return ESP_ERR_INVALID_RESPONSE;
            }

            const uint16_t format = audio_get_u16le(&fmt[0]);
            info->channels = (uint8_t)audio_get_u16le(&fmt[2]);
            info->sample_rate = audio_get_u32le(&fmt[4]);
            info->block_align = audio_get_u16le(&fmt[12]);
            info->bits_per_sample = (uint8_t)audio_get_u16le(&fmt[14]);
            if (format != AUDIO_WAV_PCM_FORMAT ||
                info->channels == 0 ||
                info->sample_rate == 0 ||
                info->block_align == 0 ||
                info->bits_per_sample == 0) {
                return ESP_ERR_NOT_SUPPORTED;
            }

            const long remaining = (long)chunk_size - (long)sizeof(fmt);
            if (remaining > 0 && fseek(file, remaining, SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
            have_fmt = true;
        } else if (memcmp(&chunk[0], "data", 4) == 0) {
            found_data_offset = chunk_data_offset;
            found_data_bytes = chunk_size;
            have_data = true;
            if (fseek(file, chunk_size + (chunk_size & 1U), SEEK_CUR) != 0) {
                return ESP_FAIL;
            }
        } else if (fseek(file, chunk_size + (chunk_size & 1U), SEEK_CUR) != 0) {
            return ESP_FAIL;
        }

        if (have_fmt && have_data) {
            info->data_bytes = found_data_bytes;
            if (info->block_align != 0 && info->sample_rate != 0) {
                const uint32_t frames = info->data_bytes / info->block_align;
                info->duration_ms = (uint32_t)(((uint64_t)frames * 1000U) / info->sample_rate);
            }
            if (data_offset != NULL) {
                *data_offset = found_data_offset;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_INVALID_RESPONSE;
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
static void *audio_heap_alloc(size_t size)
{
    return solar_os_memory_alloc(size,
                                 SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                 "audio.file");
}

static void audio_log_heap_nomem(const char *where, size_t bytes)
{
    SOLAR_OS_LOGW(TAG,
                  "%s alloc %u failed: internal free=%u largest=%u psram free=%u largest=%u",
                  where != NULL ? where : "audio",
                  (unsigned)bytes,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static uint32_t audio_mp3_synchsafe_u32(const uint8_t data[4])
{
    return ((uint32_t)(data[0] & 0x7fU) << 21) |
        ((uint32_t)(data[1] & 0x7fU) << 14) |
        ((uint32_t)(data[2] & 0x7fU) << 7) |
        (uint32_t)(data[3] & 0x7fU);
}

static esp_err_t audio_mp3_seek_payload(FILE *file)
{
    uint8_t header[10];

    if (file == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    const size_t got = fread(header, 1, sizeof(header), file);
    if (got < sizeof(header)) {
        if (ferror(file)) {
            return ESP_FAIL;
        }
        return fseek(file, 0, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    }

    if (memcmp(header, "ID3", 3) != 0) {
        return fseek(file, 0, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
    }

    uint32_t tag_bytes = audio_mp3_synchsafe_u32(&header[6]) + sizeof(header);
    if ((header[5] & 0x10U) != 0) {
        tag_bytes += 10U;
    }
    return fseek(file, (long)tag_bytes, SEEK_SET) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t audio_mp3_fill_input(FILE *file,
                                      uint8_t *input,
                                      size_t *input_len,
                                      bool *eof)
{
    if (file == NULL || input == NULL || input_len == NULL || eof == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*eof || *input_len >= AUDIO_MP3_INPUT_BUFFER_BYTES) {
        return ESP_OK;
    }

    const size_t wanted = AUDIO_MP3_INPUT_BUFFER_BYTES - *input_len;
    const size_t got = fread(input + *input_len, 1, wanted, file);
    *input_len += got;
    if (got < wanted) {
        if (ferror(file)) {
            return ESP_FAIL;
        }
        *eof = true;
    }
    return ESP_OK;
}

static void audio_mp3_consume_input(uint8_t *input, size_t *input_len, size_t consumed)
{
    if (input == NULL || input_len == NULL || consumed == 0) {
        return;
    }
    if (consumed >= *input_len) {
        *input_len = 0;
        return;
    }
    memmove(input, input + consumed, *input_len - consumed);
    *input_len -= consumed;
}

static void audio_mp3_fill_output_info(
    solar_os_audio_wav_info_t *info,
    const solar_os_stream_audio_format_t *format,
    uint32_t data_bytes)
{
    memset(info, 0, sizeof(*info));
    info->sample_rate = format->sample_rate;
    info->channels = format->channels;
    info->bits_per_sample = format->bits_per_sample;
    info->block_align = (uint16_t)(format->channels * sizeof(int16_t));
    info->data_bytes = data_bytes;
}

static esp_err_t audio_mp3_playback_flush(solar_os_audio_player_t *player,
                                          const solar_os_stream_audio_format_t *format,
                                          int16_t *playback,
                                          size_t *playback_samples,
                                          bool pad_tail)
{
    if (player == NULL || format == NULL || playback == NULL ||
        playback_samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*playback_samples == 0) {
        return ESP_OK;
    }

    if (pad_tail) {
        const size_t frames_per_block = format->frames_per_block != 0U ?
            format->frames_per_block : AUDIO_FRAME_CHUNK;
        const size_t quantum = frames_per_block * format->channels;
        size_t padded = ((*playback_samples + quantum - 1U) / quantum) * quantum;
        if (padded > AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            padded = AUDIO_MP3_OUTPUT_SAMPLES_MAX;
        }
        while (*playback_samples < padded) {
            playback[(*playback_samples)++] = 0;
        }
    }

    const size_t bytes = *playback_samples * sizeof(playback[0]);
    const esp_err_t ret = solar_os_audio_player_write(
        player, playback, bytes, NULL);
    if (ret == ESP_OK) {
        *playback_samples = 0;
    }
    return ret;
}

static esp_err_t audio_mp3_playback_append(solar_os_audio_player_t *player,
                                           const solar_os_stream_audio_format_t *format,
                                           int16_t *playback,
                                           size_t *playback_samples,
                                           const int16_t *samples,
                                           size_t sample_count)
{
    if (player == NULL || format == NULL || playback == NULL ||
        playback_samples == NULL || samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (sample_count > 0) {
        if (*playback_samples >= AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            const esp_err_t ret = audio_mp3_playback_flush(
                player, format, playback, playback_samples, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }

        size_t space = AUDIO_MP3_OUTPUT_SAMPLES_MAX - *playback_samples;
        size_t chunk = sample_count < space ? sample_count : space;
        memcpy(playback + *playback_samples, samples, chunk * sizeof(samples[0]));
        *playback_samples += chunk;
        samples += chunk;
        sample_count -= chunk;

        if (*playback_samples >= AUDIO_MP3_OUTPUT_SAMPLES_MAX) {
            const esp_err_t ret = audio_mp3_playback_flush(
                player, format, playback, playback_samples, false);
            if (ret != ESP_OK) {
                return ret;
            }
        }
    }

    return ESP_OK;
}

static esp_err_t audio_mp3_probe_from_file(FILE *file, solar_os_audio_wav_info_t *info)
{
    esp_err_t ret = audio_mp3_seek_payload(file);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t buffer[515];
    size_t scanned = 0;
    size_t carry = 0;

    while (scanned < AUDIO_MP3_PROBE_SCAN_BYTES) {
        const size_t remaining = AUDIO_MP3_PROBE_SCAN_BYTES - scanned;
        const size_t capacity = sizeof(buffer) - carry;
        const size_t wanted = remaining > capacity ? capacity : remaining;
        const size_t got = fread(buffer + carry, 1, wanted, file);
        if (got == 0) {
            if (ferror(file)) {
                return ESP_FAIL;
            }
            break;
        }

        const size_t available = carry + got;
        solar_os_stream_audio_format_t format;
        if (solar_os_audio_mp3_probe(buffer, available, &format) == ESP_OK) {
            memset(info, 0, sizeof(*info));
            info->sample_rate = format.sample_rate;
            info->channels = format.channels;
            info->bits_per_sample = format.bits_per_sample;
            info->block_align = (uint16_t)(format.channels * sizeof(int16_t));
            return ESP_OK;
        }
        scanned += got;
        carry = available < 3U ? available : 3U;
        memmove(buffer, buffer + available - carry, carry);
    }

    return ESP_ERR_INVALID_RESPONSE;
}
#endif

static bool audio_device_id_valid(const char *id)
{
    return id != NULL && id[0] != '\0' &&
        strnlen(id, SOLAR_OS_AUDIO_DEVICE_ID_MAX) < SOLAR_OS_AUDIO_DEVICE_ID_MAX;
}

esp_err_t solar_os_audio_register_device(const solar_os_audio_device_info_t *device)
{
    return solar_os_audio_register_device_ex(device, NULL, NULL);
}

esp_err_t solar_os_audio_register_device_ex(
    const solar_os_audio_device_info_t *device,
    const solar_os_audio_device_ops_t *ops,
    void *user)
{
    if (device == NULL || !audio_device_id_valid(device->id) ||
        device->name[0] == '\0' || device->provider[0] == '\0' ||
        device->capabilities == 0U || device->native_format.sample_rate == 0U ||
        device->native_format.channels == 0U ||
        device->native_format.bits_per_sample == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&audio_devices_lock);
    audio_device_entry_t *free_entry = NULL;
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            strcmp(audio_devices[i].info.id, device->id) == 0) {
            portEXIT_CRITICAL(&audio_devices_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (!audio_devices[i].registered && free_entry == NULL) {
            free_entry = &audio_devices[i];
        }
    }
    if (free_entry == NULL) {
        portEXIT_CRITICAL(&audio_devices_lock);
        return ESP_ERR_NO_MEM;
    }
    memset(free_entry, 0, sizeof(*free_entry));
    free_entry->registered = true;
    free_entry->info = *device;
    if (ops != NULL) {
        free_entry->ops = *ops;
    }
    free_entry->user = user;
    free_entry->info.id[sizeof(free_entry->info.id) - 1U] = '\0';
    free_entry->info.name[sizeof(free_entry->info.name) - 1U] = '\0';
    free_entry->info.provider[sizeof(free_entry->info.provider) - 1U] = '\0';
    free_entry->info.capture_stream[sizeof(free_entry->info.capture_stream) - 1U] = '\0';
    free_entry->info.playback_stream[sizeof(free_entry->info.playback_stream) - 1U] = '\0';
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_OK;
}

esp_err_t solar_os_audio_unregister_device(const char *id)
{
    if (!audio_device_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_stream_info_t stream;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (!audio_devices[i].registered ||
            strcmp(audio_devices[i].info.id, id) != 0) {
            continue;
        }
        const solar_os_audio_device_info_t info = audio_devices[i].info;
        portEXIT_CRITICAL(&audio_devices_lock);
        if ((info.capture_stream[0] != '\0' &&
             solar_os_stream_get_info(info.capture_stream, &stream) == ESP_OK &&
             stream.active_handles != 0U) ||
            (info.playback_stream[0] != '\0' &&
             solar_os_stream_get_info(info.playback_stream, &stream) == ESP_OK &&
             stream.active_handles != 0U)) {
            return ESP_ERR_INVALID_STATE;
        }
        portENTER_CRITICAL(&audio_devices_lock);
        if (strcmp(audio_default_output, id) == 0) {
            audio_default_output[0] = '\0';
        }
        memset(&audio_devices[i], 0, sizeof(audio_devices[i]));
        portEXIT_CRITICAL(&audio_devices_lock);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_ERR_NOT_FOUND;
}

size_t solar_os_audio_device_count(void)
{
    size_t count = 0U;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered) {
            count++;
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return count;
}

bool solar_os_audio_device_get(size_t index, solar_os_audio_device_info_t *device)
{
    if (device == NULL) {
        return false;
    }
    size_t current = 0U;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (!audio_devices[i].registered) {
            continue;
        }
        if (current++ == index) {
            *device = audio_devices[i].info;
            portEXIT_CRITICAL(&audio_devices_lock);
            return true;
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return false;
}

esp_err_t solar_os_audio_device_get_info(const char *id,
                                         solar_os_audio_device_info_t *device)
{
    if (!audio_device_id_valid(id) || device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            strcmp(audio_devices[i].info.id, id) == 0) {
            *device = audio_devices[i].info;
            portEXIT_CRITICAL(&audio_devices_lock);
            return ESP_OK;
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_ERR_NOT_FOUND;
}

bool solar_os_audio_output_available(void)
{
    bool available = false;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            (audio_devices[i].info.capabilities &
             SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) != 0U &&
            audio_devices[i].info.playback_stream[0] != '\0') {
            available = true;
            break;
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return available;
}

static bool audio_get_available_output(solar_os_audio_device_info_t *device)
{
    if (device == NULL) {
        return false;
    }

    char preferred_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    if (solar_os_audio_get_default_output(preferred_id, sizeof(preferred_id)) &&
        solar_os_audio_device_get_info(preferred_id, device) == ESP_OK &&
        (device->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) != 0U &&
        device->playback_stream[0] != '\0') {
        return true;
    }

    for (size_t index = 0; index < solar_os_audio_device_count(); index++) {
        if (solar_os_audio_device_get(index, device) &&
            (device->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) != 0U &&
            device->playback_stream[0] != '\0') {
            return true;
        }
    }
    return false;
}

esp_err_t solar_os_audio_set_default_output(const char *id)
{
    if (id == NULL || id[0] == '\0') {
        portENTER_CRITICAL(&audio_devices_lock);
        audio_default_output[0] = '\0';
        portEXIT_CRITICAL(&audio_devices_lock);
        return ESP_OK;
    }
    solar_os_audio_device_info_t device;
    const esp_err_t err = solar_os_audio_device_get_info(id, &device);
    if (err != ESP_OK) {
        return err;
    }
    if ((device.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) == 0U ||
        device.playback_stream[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if ((device.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U) {
        const esp_err_t volume_err = solar_os_audio_set_device_volume(
            device.id, audio_global_volume);
        if (volume_err != ESP_OK) {
            return volume_err;
        }
    }
    portENTER_CRITICAL(&audio_devices_lock);
    strlcpy(audio_default_output, id, sizeof(audio_default_output));
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_OK;
}

bool solar_os_audio_get_default_output(char *id, size_t id_len)
{
    if (id == NULL || id_len == 0U) {
        return false;
    }
    portENTER_CRITICAL(&audio_devices_lock);
    strlcpy(id, audio_default_output, id_len);
    const bool selected = audio_default_output[0] != '\0';
    portEXIT_CRITICAL(&audio_devices_lock);
    return selected;
}

static esp_err_t audio_set_selected_output_volume(uint8_t volume)
{
    char preferred_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    if (solar_os_audio_get_default_output(preferred_id, sizeof(preferred_id))) {
        solar_os_audio_device_info_t preferred;
        if (solar_os_audio_device_get_info(preferred_id, &preferred) == ESP_OK &&
            (preferred.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) != 0U &&
            preferred.playback_stream[0] != '\0') {
            return (preferred.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U ?
                solar_os_audio_set_device_volume(preferred.id, volume) :
                ESP_ERR_NOT_SUPPORTED;
        }
    }

    for (size_t index = 0; index < solar_os_audio_device_count(); index++) {
        solar_os_audio_device_info_t candidate;
        if (!solar_os_audio_device_get(index, &candidate) ||
            (candidate.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT) == 0U ||
            candidate.playback_stream[0] == '\0') {
            continue;
        }
        return (candidate.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U ?
            solar_os_audio_set_device_volume(candidate.id, volume) :
            ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t audio_open_candidate(
    const solar_os_audio_device_info_t *candidate,
    solar_os_stream_direction_t direction,
    const char *owner,
    const solar_os_stream_open_options_t *requested,
    solar_os_stream_handle_t *stream,
    solar_os_audio_device_info_t *device)
{
    const uint32_t required_capability =
        direction == SOLAR_OS_STREAM_DIRECTION_SOURCE ?
        SOLAR_OS_AUDIO_DEVICE_CAP_INPUT : SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT;
    if (candidate == NULL ||
        (candidate->capabilities & required_capability) == 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const char *endpoint = direction == SOLAR_OS_STREAM_DIRECTION_SOURCE ?
        candidate->capture_stream : candidate->playback_stream;
    if (endpoint[0] == '\0') {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t err = solar_os_stream_open_ex(
        endpoint, owner, requested, stream);
    if (err == ESP_OK && direction == SOLAR_OS_STREAM_DIRECTION_SINK &&
        (candidate->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME) != 0U) {
        err = solar_os_audio_set_device_volume(candidate->id,
                                               audio_global_volume);
        if (err != ESP_OK) {
            solar_os_stream_close(stream);
        }
    }
    if (err == ESP_OK && device != NULL) {
        *device = *candidate;
    }
    return err;
}

esp_err_t solar_os_audio_open_default(
    solar_os_stream_direction_t direction,
    const char *owner,
    const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *stream,
    solar_os_audio_device_info_t *device)
{
    if ((direction != SOLAR_OS_STREAM_DIRECTION_SOURCE &&
         direction != SOLAR_OS_STREAM_DIRECTION_SINK) ||
        owner == NULL || owner[0] == '\0' || stream == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_stream_open_options_t requested = options != NULL ?
        *options : (solar_os_stream_open_options_t){0};
    requested.direction = direction;

    char preferred_id[SOLAR_OS_AUDIO_DEVICE_ID_MAX];
    if (direction == SOLAR_OS_STREAM_DIRECTION_SINK &&
        solar_os_audio_get_default_output(preferred_id, sizeof(preferred_id))) {
        solar_os_audio_device_info_t preferred;
        if (solar_os_audio_device_get_info(preferred_id, &preferred) == ESP_OK) {
            const esp_err_t err = audio_open_candidate(
                &preferred, direction, owner, &requested, stream, device);
            if (err == ESP_OK) {
                return ESP_OK;
            }
            if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED &&
                err != ESP_ERR_INVALID_STATE) {
                return err;
            }
        }
    } else {
        preferred_id[0] = '\0';
    }

    for (size_t index = 0; index < solar_os_audio_device_count(); index++) {
        solar_os_audio_device_info_t candidate;
        if (!solar_os_audio_device_get(index, &candidate) ||
            strcmp(candidate.id, preferred_id) == 0) {
            continue;
        }
        const esp_err_t err = audio_open_candidate(
            &candidate, direction, owner, &requested, stream, device);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_NOT_SUPPORTED &&
            err != ESP_ERR_INVALID_STATE) {
            return err;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_audio_capture(const char *owner,
                                 size_t frames,
                                 int16_t *samples,
                                 size_t sample_capacity,
                                 solar_os_audio_stream_format_t *format)
{
    if (owner == NULL || owner[0] == '\0' || frames == 0U ||
        frames > SOLAR_OS_AUDIO_CAPTURE_MAX_FRAMES || samples == NULL ||
        format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(format, 0, sizeof(*format));

    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = 0U,
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
    };
    solar_os_stream_handle_t stream = SOLAR_OS_STREAM_HANDLE_INIT;
    esp_err_t err = solar_os_audio_open_default(
        SOLAR_OS_STREAM_DIRECTION_SOURCE, owner, &options, &stream, NULL);
    if (err != ESP_OK) {
        return err;
    }

    const solar_os_audio_stream_format_t captured_format = stream.audio;
    if (captured_format.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        captured_format.bits_per_sample != 16U ||
        captured_format.sample_rate == 0U || captured_format.channels == 0U ||
        captured_format.channels > SOLAR_OS_AUDIO_CAPTURE_MAX_CHANNELS) {
        solar_os_stream_close(&stream);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const size_t sample_count = frames * captured_format.channels;
    if (sample_capacity < sample_count) {
        solar_os_stream_close(&stream);
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t target_bytes = sample_count * sizeof(*samples);
    const size_t frame_bytes =
        captured_format.channels * sizeof(*samples);
    size_t captured_bytes = 0U;
    while (captured_bytes < target_bytes) {
        const size_t remaining_frames =
            (target_bytes - captured_bytes) / frame_bytes;
        size_t block_frames = captured_format.frames_per_block;
        if (block_frames == 0U || block_frames > remaining_frames) {
            block_frames = remaining_frames;
        }
        const size_t read_bytes = block_frames * frame_bytes;
        size_t read_len = 0U;
        err = solar_os_stream_read(&stream,
                                   (uint8_t *)samples + captured_bytes,
                                   read_bytes,
                                   UINT32_MAX,
                                   &read_len);
        if (err != ESP_OK) {
            break;
        }
        if (read_len == 0U || read_len > read_bytes ||
            (read_len % frame_bytes) != 0U) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        captured_bytes += read_len;
    }

    solar_os_stream_close(&stream);
    if (err == ESP_OK) {
        *format = captured_format;
    }
    return err;
}

esp_err_t solar_os_audio_set_device_volume(const char *id, uint8_t volume)
{
    if (!audio_device_id_valid(id) || volume > 100U) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_audio_device_ops_t ops = {0};
    void *user = NULL;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            strcmp(audio_devices[i].info.id, id) == 0) {
            ops = audio_devices[i].ops;
            user = audio_devices[i].user;
            portEXIT_CRITICAL(&audio_devices_lock);
            return ops.set_volume != NULL ?
                ops.set_volume(user, volume) : ESP_ERR_NOT_SUPPORTED;
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_audio_set_device_input_gain(const char *id, float gain_db)
{
    if (!audio_device_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_audio_device_ops_t ops = {0};
    solar_os_audio_device_info_t info = {0};
    void *user = NULL;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            strcmp(audio_devices[i].info.id, id) == 0) {
            ops = audio_devices[i].ops;
            info = audio_devices[i].info;
            user = audio_devices[i].user;
            portEXIT_CRITICAL(&audio_devices_lock);
            if ((info.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN) == 0U ||
                ops.set_input_gain == NULL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (info.input_gain_max_db > info.input_gain_min_db &&
                (gain_db < info.input_gain_min_db ||
                 gain_db > info.input_gain_max_db)) {
                return ESP_ERR_INVALID_ARG;
            }
            return ops.set_input_gain(user, gain_db);
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_audio_get_device_input_gain(const char *id, float *gain_db)
{
    if (!audio_device_id_valid(id) || gain_db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_audio_device_ops_t ops = {0};
    solar_os_audio_device_info_t info = {0};
    void *user = NULL;
    portENTER_CRITICAL(&audio_devices_lock);
    for (size_t i = 0; i < SOLAR_OS_AUDIO_DEVICE_MAX; i++) {
        if (audio_devices[i].registered &&
            strcmp(audio_devices[i].info.id, id) == 0) {
            ops = audio_devices[i].ops;
            info = audio_devices[i].info;
            user = audio_devices[i].user;
            portEXIT_CRITICAL(&audio_devices_lock);
            if ((info.capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN) == 0U ||
                ops.get_input_gain == NULL) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            return ops.get_input_gain(user, gain_db);
        }
    }
    portEXIT_CRITICAL(&audio_devices_lock);
    return ESP_ERR_NOT_FOUND;
}

#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
esp_err_t solar_os_audio_init(void)
{
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_operation_take(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = solar_os_board_audio_init();
    audio_operation_give();
    return ret;
#endif
}

void solar_os_audio_deinit(void)
{
#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
    audio_tone_cancel_all();
    if (audio_operation_take(portMAX_DELAY) == ESP_OK) {
        if (!audio_output_stream.active && !audio_input_stream.active) {
            solar_os_board_audio_deinit();
        }
        audio_operation_give();
    }
#endif
}

esp_err_t solar_os_audio_set_volume(uint8_t volume)
{
    if (volume > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = audio_set_selected_output_volume(volume);
    if (ret == ESP_OK) {
        audio_global_volume = volume;
        if (volume > 0) {
            audio_mute_restore_volume = volume;
        }
    }
    return ret;
}

static esp_err_t audio_apply_playback_volume(uint8_t volume)
{
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_audio_set_volume(audio_resolve_playback_volume(volume));
#endif
}

esp_err_t solar_os_audio_toggle_mute(uint8_t *volume_after)
{
    uint8_t target = audio_mute_restore_volume;
    if (audio_global_volume > 0) {
        audio_mute_restore_volume = audio_global_volume;
        target = 0;
    } else if (target == 0 || target > 100) {
        target = AUDIO_GLOBAL_DEFAULT_VOLUME;
    }

    const esp_err_t ret = solar_os_audio_set_volume(target);
    if (volume_after != NULL) {
        *volume_after = ret == ESP_OK ? target : audio_global_volume;
    }
    return ret;
}

esp_err_t solar_os_audio_set_mic_gain(float gain_db)
{
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
#else
    return solar_os_board_audio_set_mic_gain(gain_db);
#endif
}

esp_err_t solar_os_audio_stream_open(const char *owner,
                                     uint32_t timeout_ms,
                                     solar_os_audio_stream_t **stream,
                                     solar_os_audio_stream_format_t *format)
{
    if (owner == NULL || owner[0] == '\0' || stream == NULL || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream = NULL;
    memset(format, 0, sizeof(*format));
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_operation_take(audio_timeout_ticks(timeout_ms));
    if (ret != ESP_OK) {
        return ret;
    }
    if (audio_output_stream.active) {
        audio_operation_give();
        return ESP_ERR_INVALID_STATE;
    }

    ret = solar_os_board_audio_init();
    if (ret != ESP_OK) {
        audio_operation_give();
        return ret;
    }
    ret = audio_apply_playback_volume(SOLAR_OS_AUDIO_VOLUME_GLOBAL);
    if (ret != ESP_OK) {
        audio_board_deinit_if_unused();
        audio_operation_give();
        return ret;
    }

    solar_os_board_audio_status_t status;
    solar_os_board_audio_get_status(&status);
    if (!status.initialized || status.sample_rate == 0 || status.channels == 0 ||
        status.bits_per_sample == 0) {
        audio_board_deinit_if_unused();
        audio_operation_give();
        return ESP_ERR_INVALID_STATE;
    }

    audio_output_stream.task = xTaskGetCurrentTaskHandle();
    audio_output_stream.active = true;
    strlcpy(audio_output_stream.owner, owner, sizeof(audio_output_stream.owner));
    *format = (solar_os_audio_stream_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = status.sample_rate,
        .channels = status.channels,
        .bits_per_sample = status.bits_per_sample,
        .frames_per_block = AUDIO_FRAME_CHUNK,
    };
    *stream = &audio_output_stream;
    SOLAR_OS_LOGI(TAG, "stream open: owner=%s %" PRIu32 "Hz %uch %ubit",
                  audio_output_stream.owner, format->sample_rate,
                  (unsigned)format->channels,
                  (unsigned)format->bits_per_sample);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_stream_write(solar_os_audio_stream_t *stream,
                                      const void *data,
                                      size_t len)
{
    if (stream == NULL || data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (stream != &audio_output_stream || !stream->active ||
        stream->task != xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_board_audio_write(data, len);
#endif
}

void solar_os_audio_stream_close(solar_os_audio_stream_t *stream)
{
#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
    if (stream != &audio_output_stream || !stream->active ||
        stream->task != xTaskGetCurrentTaskHandle()) {
        return;
    }
    SOLAR_OS_LOGI(TAG, "stream close: owner=%s", stream->owner);
    stream->active = false;
    stream->task = NULL;
    stream->owner[0] = '\0';
    audio_board_deinit_if_unused();
    audio_operation_give();
#else
    (void)stream;
#endif
}

esp_err_t solar_os_audio_input_stream_open(const char *owner,
                                           uint32_t timeout_ms,
                                           solar_os_audio_input_stream_t **stream,
                                           solar_os_audio_stream_format_t *format)
{
    if (owner == NULL || owner[0] == '\0' || stream == NULL || format == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *stream = NULL;
    memset(format, 0, sizeof(*format));
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!solar_os_audio_backend_has_input()) {
        (void)timeout_ms;
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = audio_operation_take(audio_timeout_ticks(timeout_ms));
    if (ret != ESP_OK) {
        return ret;
    }
    if (audio_input_stream.active) {
        audio_operation_give();
        return ESP_ERR_INVALID_STATE;
    }
    ret = solar_os_board_audio_init();
    if (ret != ESP_OK) {
        audio_operation_give();
        return ret;
    }
    solar_os_board_audio_status_t status;
    solar_os_board_audio_get_status(&status);
    if (!status.initialized || status.sample_rate == 0U || status.channels == 0U ||
        status.bits_per_sample == 0U) {
        audio_board_deinit_if_unused();
        audio_operation_give();
        return ESP_ERR_INVALID_STATE;
    }
    audio_input_stream.task = xTaskGetCurrentTaskHandle();
    audio_input_stream.active = true;
    strlcpy(audio_input_stream.owner, owner, sizeof(audio_input_stream.owner));
    *format = (solar_os_audio_stream_format_t){
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = status.sample_rate,
        .channels = status.channels,
        .bits_per_sample = status.bits_per_sample,
        .frames_per_block = AUDIO_FRAME_CHUNK,
    };
    *stream = &audio_input_stream;
    SOLAR_OS_LOGI(TAG, "input stream open: owner=%s %" PRIu32 "Hz %uch %ubit",
                  audio_input_stream.owner, format->sample_rate,
                  (unsigned)format->channels,
                  (unsigned)format->bits_per_sample);
    /* Capture and playback have independent I2S channels. Keep capture
     * ownership in audio_input_stream, but release the operation lock after
     * the state transition so an output worker can open the duplex sink. */
    audio_operation_give();
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_input_stream_read(solar_os_audio_input_stream_t *stream,
                                           void *data,
                                           size_t len)
{
    if (stream == NULL || data == NULL || len == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (stream != &audio_input_stream || !stream->active ||
        stream->task != xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }
    return solar_os_board_audio_read(data, len);
#endif
}

void solar_os_audio_input_stream_close(solar_os_audio_input_stream_t *stream)
{
#if SOLAR_OS_AUDIO_BACKEND_PACKAGE
    if (stream != &audio_input_stream || !stream->active ||
        stream->task != xTaskGetCurrentTaskHandle()) {
        return;
    }
    if (audio_operation_take(portMAX_DELAY) != ESP_OK) {
        return;
    }
    SOLAR_OS_LOGI(TAG, "input stream close: owner=%s", stream->owner);
    stream->active = false;
    stream->task = NULL;
    stream->owner[0] = '\0';
    audio_board_deinit_if_unused();
    audio_operation_give();
#else
    (void)stream;
#endif
}

static bool audio_stream_format_requested(
    const solar_os_stream_audio_format_t *format)
{
    return format != NULL &&
        (format->sample_rate != 0U || format->channels != 0U ||
         format->bits_per_sample != 0U);
}

static bool audio_stream_format_matches(
    const solar_os_stream_audio_format_t *requested,
    const solar_os_stream_audio_format_t *actual)
{
    if (!audio_stream_format_requested(requested)) {
        return true;
    }
    return (requested->sample_rate == 0U ||
            requested->sample_rate == actual->sample_rate) &&
           (requested->channels == 0U || requested->channels == actual->channels) &&
           (requested->bits_per_sample == 0U ||
            requested->bits_per_sample == actual->bits_per_sample) &&
           requested->sample_format == actual->sample_format;
}

static esp_err_t audio_playback_stream_open(
    void *user,
    const char *owner,
    const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_audio_stream_t *stream = NULL;
    solar_os_audio_stream_format_t format;
    esp_err_t err = solar_os_audio_stream_open(
        owner, options != NULL ? options->timeout_ms : 0U, &stream, &format);
    if (err != ESP_OK) {
        return err;
    }
    if (options != NULL &&
        !audio_stream_format_matches(&options->requested_audio, &format)) {
        solar_os_audio_stream_close(stream);
        return ESP_ERR_NOT_SUPPORTED;
    }
    handle->context = stream;
    handle->audio = format;
    return ESP_OK;
}

static void audio_playback_stream_close(void *user,
                                        solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_audio_stream_close(handle->context);
    handle->context = NULL;
}

static esp_err_t audio_playback_stream_write(void *user,
                                             solar_os_stream_handle_t *handle,
                                             const void *data,
                                             size_t len,
                                             uint32_t timeout_ms,
                                             size_t *written)
{
    (void)user;
    (void)timeout_ms;
    const esp_err_t err = solar_os_audio_stream_write(handle->context, data, len);
    if (err == ESP_OK) {
        *written = len;
    }
    return err;
}

static esp_err_t audio_capture_stream_open(
    void *user,
    const char *owner,
    const solar_os_stream_open_options_t *options,
    solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_audio_input_stream_t *stream = NULL;
    solar_os_audio_stream_format_t format;
    esp_err_t err = solar_os_audio_input_stream_open(
        owner, options != NULL ? options->timeout_ms : 0U, &stream, &format);
    if (err != ESP_OK) {
        return err;
    }
    if (options != NULL &&
        !audio_stream_format_matches(&options->requested_audio, &format)) {
        solar_os_audio_input_stream_close(stream);
        return ESP_ERR_NOT_SUPPORTED;
    }
    handle->context = stream;
    handle->audio = format;
    return ESP_OK;
}

static void audio_capture_stream_close(void *user,
                                       solar_os_stream_handle_t *handle)
{
    (void)user;
    solar_os_audio_input_stream_close(handle->context);
    handle->context = NULL;
}

static esp_err_t audio_capture_stream_read(void *user,
                                           solar_os_stream_handle_t *handle,
                                           void *data,
                                           size_t len,
                                           uint32_t timeout_ms,
                                           size_t *read_len)
{
    (void)user;
    (void)timeout_ms;
    const esp_err_t err = solar_os_audio_input_stream_read(
        handle->context, data, len);
    if (err == ESP_OK) {
        *read_len = len;
    }
    return err;
}

static esp_err_t audio_level_stream_read_scalar(
    void *user,
    const solar_os_stream_read_options_t *options,
    float *value)
{
    if (value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t window_ms = 100U;
    if (options != NULL && options->window_ms != 0U) {
        window_ms = options->window_ms;
    }
    solar_os_audio_level_t level;
    const esp_err_t err = solar_os_audio_measure_channel_level(
        (uint8_t)(uintptr_t)user, window_ms, &level);
    if (err == ESP_OK) {
        *value = (float)level.average_percent;
    }
    return err;
}

static esp_err_t audio_register_endpoint(
    const char *id,
    const char *device_id,
    solar_os_stream_type_t type,
    solar_os_stream_direction_t direction,
    solar_os_stream_sharing_t sharing,
    const char *unit,
    const char *format,
    const char *summary,
    const solar_os_stream_audio_format_t *audio_format,
    solar_os_stream_open_fn open,
    solar_os_stream_close_fn close,
    solar_os_stream_read_fn read,
    solar_os_stream_write_fn write,
    solar_os_stream_read_scalar_fn read_scalar,
    void *user)
{
    solar_os_stream_driver_t driver = {
        .info = {
            .type = type,
            .direction = direction,
            .sharing = sharing,
        },
        .open = open,
        .close = close,
        .read = read,
        .write = write,
        .read_scalar = read_scalar,
        .user = user,
    };
    strlcpy(driver.info.id, id, sizeof(driver.info.id));
    strlcpy(driver.info.provider, "audio", sizeof(driver.info.provider));
    strlcpy(driver.info.device, device_id, sizeof(driver.info.device));
    strlcpy(driver.info.unit, unit != NULL ? unit : "", sizeof(driver.info.unit));
    strlcpy(driver.info.format, format, sizeof(driver.info.format));
    strlcpy(driver.info.summary, summary, sizeof(driver.info.summary));
    if (audio_format != NULL) {
        driver.info.audio = *audio_format;
    }
    return solar_os_stream_register(&driver);
}

static esp_err_t audio_board_device_set_volume(void *user, uint8_t volume)
{
    (void)user;
    return audio_apply_playback_volume(volume);
}

static esp_err_t audio_board_device_set_input_gain(void *user, float gain_db)
{
    (void)user;
    return solar_os_audio_set_mic_gain(gain_db);
}

static esp_err_t audio_board_device_get_input_gain(void *user, float *gain_db)
{
    (void)user;
    if (gain_db == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_board_audio_status_t status;
    solar_os_board_audio_get_status(&status);
    *gain_db = status.mic_gain_db;
    return ESP_OK;
}

esp_err_t solar_os_audio_register_streams(void)
{
    if (!solar_os_audio_backend_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    solar_os_audio_backend_info_t backend_info;
    solar_os_audio_backend_get_info(&backend_info);
    solar_os_audio_backend_status_t backend_status;
    solar_os_audio_backend_get_status(&backend_status);
    if (backend_info.id == NULL || backend_info.name == NULL ||
        backend_status.sample_rate == 0U || backend_status.channels == 0U ||
        backend_status.bits_per_sample == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    char playback_id[SOLAR_OS_STREAM_ID_MAX];
    char capture_id[SOLAR_OS_STREAM_ID_MAX];
    const int playback_len = snprintf(playback_id, sizeof(playback_id),
                                      "%s.playback", backend_info.id);
    const int capture_len = snprintf(capture_id, sizeof(capture_id),
                                     "%s.capture", backend_info.id);
    if (playback_len < 0 || (size_t)playback_len >= sizeof(playback_id) ||
        capture_len < 0 || (size_t)capture_len >= sizeof(capture_id)) {
        return ESP_ERR_INVALID_ARG;
    }
    const solar_os_stream_audio_format_t native_format = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = backend_status.sample_rate,
        .channels = backend_status.channels,
        .bits_per_sample = backend_status.bits_per_sample,
        .frames_per_block = AUDIO_FRAME_CHUNK,
    };
    solar_os_audio_device_info_t device = {
        .capabilities = SOLAR_OS_AUDIO_DEVICE_CAP_OUTPUT |
                        SOLAR_OS_AUDIO_DEVICE_CAP_VOLUME,
        .native_format = native_format,
    };
    strlcpy(device.id, backend_info.id, sizeof(device.id));
    strlcpy(device.name, backend_info.name, sizeof(device.name));
    strlcpy(device.provider, "expansion", sizeof(device.provider));
    strlcpy(device.playback_stream, playback_id,
            sizeof(device.playback_stream));
    if (backend_info.has_input) {
        device.capabilities |= SOLAR_OS_AUDIO_DEVICE_CAP_INPUT |
                               SOLAR_OS_AUDIO_DEVICE_CAP_INPUT_GAIN;
        strlcpy(device.capture_stream, capture_id,
                sizeof(device.capture_stream));
        device.input_gain_min_db = 0.0f;
        device.input_gain_max_db = 37.5f;
        device.input_gain_step_db = 3.0f;
    }
    const solar_os_audio_device_ops_t device_ops = {
        .set_volume = audio_board_device_set_volume,
        .set_input_gain = audio_board_device_set_input_gain,
        .get_input_gain = audio_board_device_get_input_gain,
    };
    esp_err_t err = solar_os_audio_register_device_ex(
        &device, &device_ops, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = audio_register_endpoint(
        playback_id, backend_info.id, SOLAR_OS_STREAM_TYPE_AUDIO,
        SOLAR_OS_STREAM_DIRECTION_SINK, SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
        "frames", "pcm-s16le", "primary audio playback",
        &native_format, audio_playback_stream_open,
        audio_playback_stream_close, NULL, audio_playback_stream_write,
        NULL, NULL);
    if (err != ESP_OK) {
        (void)solar_os_audio_unregister_device(backend_info.id);
        return err;
    }
    if (backend_info.has_input) {
        err = audio_register_endpoint(
            capture_id, backend_info.id, SOLAR_OS_STREAM_TYPE_AUDIO,
            SOLAR_OS_STREAM_DIRECTION_SOURCE,
            SOLAR_OS_STREAM_SHARING_EXCLUSIVE,
            "frames", "pcm-s16le", "primary audio capture",
            &native_format, audio_capture_stream_open,
            audio_capture_stream_close, audio_capture_stream_read, NULL,
            NULL, NULL);
        if (err == ESP_OK) {
            err = audio_register_endpoint(
                "mic0", backend_info.id, SOLAR_OS_STREAM_TYPE_SCALAR,
                SOLAR_OS_STREAM_DIRECTION_SOURCE,
                SOLAR_OS_STREAM_SHARING_SHARED,
                "percent", "f32", "left microphone level", NULL,
                NULL, NULL, NULL, NULL, audio_level_stream_read_scalar,
                (void *)(uintptr_t)0U);
        }
        if (err == ESP_OK && backend_status.channels > 1U) {
            err = audio_register_endpoint(
                "mic1", backend_info.id, SOLAR_OS_STREAM_TYPE_SCALAR,
                SOLAR_OS_STREAM_DIRECTION_SOURCE,
                SOLAR_OS_STREAM_SHARING_SHARED,
                "percent", "f32", "right microphone level", NULL,
                NULL, NULL, NULL, NULL, audio_level_stream_read_scalar,
                (void *)(uintptr_t)1U);
        }
        if (err != ESP_OK) {
            (void)solar_os_stream_unregister("mic1");
            (void)solar_os_stream_unregister("mic0");
            (void)solar_os_stream_unregister(capture_id);
            (void)solar_os_stream_unregister(playback_id);
            (void)solar_os_audio_unregister_device(backend_info.id);
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t audio_stream_require_idle(const char *id)
{
    solar_os_stream_info_t stream;
    const esp_err_t err = solar_os_stream_get_info(id, &stream);
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    if (err != ESP_OK) {
        return err;
    }
    return stream.active_handles == 0U ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_audio_unregister_streams(const char *id)
{
    if (!audio_device_id_valid(id)) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_audio_device_info_t device;
    esp_err_t err = solar_os_audio_device_get_info(id, &device);
    if (err != ESP_OK) {
        return err;
    }
    err = audio_stream_require_idle(device.playback_stream);
    if (err != ESP_OK) {
        return err;
    }
    if (device.capture_stream[0] != '\0') {
        err = audio_stream_require_idle(device.capture_stream);
        if (err != ESP_OK) {
            return err;
        }
        err = audio_stream_require_idle("mic0");
        if (err != ESP_OK) {
            return err;
        }
        err = audio_stream_require_idle("mic1");
        if (err != ESP_OK) {
            return err;
        }
        err = solar_os_stream_unregister("mic1");
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        err = solar_os_stream_unregister("mic0");
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            return err;
        }
        err = solar_os_stream_unregister(device.capture_stream);
        if (err != ESP_OK) {
            return err;
        }
    }
    err = solar_os_stream_unregister(device.playback_stream);
    if (err != ESP_OK) {
        return err;
    }
    return solar_os_audio_unregister_device(id);
}

static esp_err_t audio_play_tone_locked(uint32_t frequency_hz,
                                        uint32_t duration_ms,
                                        uint8_t volume,
                                        const volatile bool *cancelled)
{
    if (frequency_hz < SOLAR_OS_AUDIO_TONE_MIN_HZ ||
        frequency_hz > SOLAR_OS_AUDIO_TONE_MAX_HZ ||
        duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        !audio_volume_arg_valid(volume)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_apply_playback_volume(volume);
    if (ret != ESP_OK) {
        return ret;
    }

    solar_os_board_audio_status_t status;
    solar_os_board_audio_get_status(&status);
    if (status.sample_rate == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int16_t samples[AUDIO_FRAME_CHUNK * SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS];
    uint32_t phase = 0;
    const uint32_t phase_step = (uint32_t)(((uint64_t)frequency_hz << 16) / status.sample_rate);
    uint32_t frames_remaining = (uint32_t)(((uint64_t)status.sample_rate * duration_ms) / 1000U);

    while (frames_remaining > 0) {
        if (cancelled != NULL && *cancelled) {
            return ESP_ERR_TIMEOUT;
        }
        const uint32_t frames = frames_remaining > AUDIO_FRAME_CHUNK ?
            AUDIO_FRAME_CHUNK :
            frames_remaining;
        for (uint32_t frame = 0; frame < frames; frame++) {
            const int16_t sample = (phase & 0x8000U) ? AUDIO_TONE_AMPLITUDE : -AUDIO_TONE_AMPLITUDE;
            phase += phase_step;
            for (uint8_t ch = 0; ch < SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS; ch++) {
                samples[(frame * SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS) + ch] = sample;
            }
        }

        ret = solar_os_board_audio_write(samples,
                                      frames * SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS * sizeof(samples[0]));
        if (ret != ESP_OK) {
            return ret;
        }
        frames_remaining -= frames;
    }

    memset(samples, 0, sizeof(samples));
    (void)solar_os_board_audio_write(samples, sizeof(samples));
    SOLAR_OS_LOGI(TAG, "tone: %" PRIu32 " Hz %" PRIu32 " ms vol=%u",
             frequency_hz,
             duration_ms,
             volume);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz,
                                   uint32_t duration_ms,
                                   uint8_t volume)
{
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    (void)frequency_hz;
    (void)duration_ms;
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_operation_take(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_play_tone_locked(frequency_hz, duration_ms, volume, NULL);
    audio_operation_give();
    return ret;
#endif
}

static bool audio_tone_request_valid(const solar_os_audio_tone_request_t *request)
{
    if (request == NULL || request->steps == NULL || request->step_count == 0 ||
        request->step_count > SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_STEPS ||
        !audio_volume_arg_valid(request->volume)) {
        return false;
    }

    uint64_t total_ms = 0;
    for (size_t i = 0; i < request->step_count; i++) {
        const solar_os_audio_tone_step_t *step = &request->steps[i];
        if (step->frequency_hz < SOLAR_OS_AUDIO_TONE_MIN_HZ ||
            step->frequency_hz > SOLAR_OS_AUDIO_TONE_MAX_HZ ||
            step->duration_ms == 0 ||
            step->duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS) {
            return false;
        }
        total_ms += step->duration_ms;
        if (i + 1U < request->step_count) {
            total_ms += step->pause_ms;
        }
    }
    return total_ms <= SOLAR_OS_AUDIO_TONE_SEQUENCE_MAX_MS;
}

static bool audio_tone_cancelled(void)
{
    return audio_tones.cancel_current;
}

static bool audio_tone_delay(uint32_t duration_ms)
{
    while (duration_ms > 0 && !audio_tone_cancelled()) {
        const uint32_t delay_ms = duration_ms > AUDIO_TONE_CANCEL_POLL_MS ?
            AUDIO_TONE_CANCEL_POLL_MS : duration_ms;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        duration_ms -= delay_ms;
    }
    return !audio_tone_cancelled();
}

static void audio_tone_finish(const audio_tone_queue_entry_t *entry,
                              esp_err_t result,
                              bool dropped)
{
    audio_tone_lock();
    if (audio_tones.cancel_current || result == ESP_ERR_TIMEOUT) {
        audio_tones.cancelled++;
    } else if (dropped) {
        audio_tones.dropped++;
    } else if (result == ESP_OK) {
        audio_tones.completed++;
    } else {
        audio_tones.failed++;
    }
    audio_tones.current_id = 0;
    audio_tones.cancel_current = false;
    audio_tones.playing = false;
    audio_tone_unlock();

    if (result != ESP_OK && result != ESP_ERR_TIMEOUT && !dropped) {
        SOLAR_OS_LOGW(TAG,
                      "async tone %" PRIu32 " failed: %s",
                      entry->id,
                      esp_err_to_name(result));
    }
}

static void audio_tone_worker(void *arg)
{
    (void)arg;

    for (;;) {
        audio_tone_queue_entry_t entry;
        bool have_entry = false;

        audio_tone_lock();
        if (audio_tones.count > 0) {
            entry = audio_tones.entries[0];
            for (size_t i = 1; i < audio_tones.count; i++) {
                audio_tones.entries[i - 1U] = audio_tones.entries[i];
            }
            audio_tones.count--;
            audio_tones.current_id = entry.id;
            audio_tones.cancel_current = false;
            have_entry = true;
        }
        audio_tone_unlock();

        if (!have_entry) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        const TickType_t wait = entry.drop_if_busy ? 0 : portMAX_DELAY;
        esp_err_t result = audio_operation_take(wait);
        const bool dropped = result != ESP_OK && entry.drop_if_busy;
        if (result == ESP_OK) {
            audio_tone_lock();
            audio_tones.playing = true;
            audio_tone_unlock();

            if (audio_resolve_playback_volume(entry.volume) == 0) {
                result = ESP_OK;
            } else {
                for (size_t i = 0; i < entry.step_count; i++) {
                    if (audio_tone_cancelled()) {
                        result = ESP_ERR_TIMEOUT;
                        break;
                    }
                    result = audio_play_tone_locked(entry.steps[i].frequency_hz,
                                                    entry.steps[i].duration_ms,
                                                    entry.volume,
                                                    &audio_tones.cancel_current);
                    if (result != ESP_OK) {
                        break;
                    }
                    if (i + 1U < entry.step_count &&
                        !audio_tone_delay(entry.steps[i].pause_ms)) {
                        result = ESP_ERR_TIMEOUT;
                        break;
                    }
                }
            }
            audio_operation_give();
        }
        audio_tone_finish(&entry, result, dropped);
    }
}

static esp_err_t audio_tone_start_worker_locked(void)
{
    if (audio_tones.task != NULL) {
        return ESP_OK;
    }
    return solar_os_task_create_pinned_internal(audio_tone_worker,
                                                 "audio_tones",
                                                 AUDIO_TONE_WORKER_STACK,
                                                 NULL,
                                                 AUDIO_TONE_WORKER_PRIORITY,
                                                 &audio_tones.task,
                                                 tskNO_AFFINITY,
                                                 SOLAR_OS_TASK_ROLE_SYSTEM) == pdPASS ?
        ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t solar_os_audio_tone_enqueue(const solar_os_audio_tone_request_t *request,
                                      uint32_t *request_id)
{
    if (request_id != NULL) {
        *request_id = 0;
    }
    if (!audio_tone_request_valid(request)) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_ensure_mutexes();
    if (ret != ESP_OK) {
        return ret;
    }

    audio_tone_lock();
    ret = audio_tone_start_worker_locked();
    if (ret != ESP_OK) {
        audio_tone_unlock();
        return ret;
    }
    if (request->drop_if_busy &&
        (audio_tones.current_id != 0 || audio_tones.count > 0)) {
        audio_tones.dropped++;
        audio_tone_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (audio_tones.count >= SOLAR_OS_AUDIO_TONE_QUEUE_CAPACITY) {
        audio_tones.dropped++;
        audio_tone_unlock();
        return ESP_ERR_NO_MEM;
    }

    audio_tone_queue_entry_t *entry = &audio_tones.entries[audio_tones.count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = audio_tones.next_id++;
    if (audio_tones.next_id == 0) {
        audio_tones.next_id = 1U;
    }
    entry->step_count = request->step_count;
    entry->volume = request->volume;
    entry->drop_if_busy = request->drop_if_busy;
    memcpy(entry->steps, request->steps, request->step_count * sizeof(entry->steps[0]));
    if (request_id != NULL) {
        *request_id = entry->id;
    }
    const TaskHandle_t task = audio_tones.task;
    audio_tone_unlock();
    xTaskNotifyGive(task);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_tone_cancel(uint32_t request_id)
{
    if (request_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (audio_ensure_mutexes() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    audio_tone_lock();
    if (audio_tones.current_id == request_id) {
        audio_tones.cancel_current = true;
        const TaskHandle_t task = audio_tones.task;
        audio_tone_unlock();
        if (task != NULL) {
            xTaskNotifyGive(task);
        }
        return ESP_OK;
    }
    for (size_t i = 0; i < audio_tones.count; i++) {
        if (audio_tones.entries[i].id != request_id) {
            continue;
        }
        for (size_t next = i + 1U; next < audio_tones.count; next++) {
            audio_tones.entries[next - 1U] = audio_tones.entries[next];
        }
        audio_tones.count--;
        audio_tones.cancelled++;
        audio_tone_unlock();
        return ESP_OK;
    }
    audio_tone_unlock();
    return ESP_ERR_NOT_FOUND;
#endif
}

static void audio_tone_cancel_all(void)
{
    if (audio_ensure_mutexes() != ESP_OK) {
        return;
    }
    audio_tone_lock();
    audio_tones.cancelled += audio_tones.count;
    audio_tones.count = 0;
    audio_tones.cancel_current = audio_tones.current_id != 0;
    const TaskHandle_t task = audio_tones.task;
    audio_tone_unlock();
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

void solar_os_audio_tone_queue_get_status(solar_os_audio_tone_queue_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (audio_ensure_mutexes() != ESP_OK) {
        return;
    }
    audio_tone_lock();
    *status = (solar_os_audio_tone_queue_status_t){
        .worker_running = audio_tones.task != NULL,
        .playing = audio_tones.playing,
        .queued = audio_tones.count,
        .current_id = audio_tones.current_id,
        .completed = audio_tones.completed,
        .cancelled = audio_tones.cancelled,
        .dropped = audio_tones.dropped,
        .failed = audio_tones.failed,
    };
    audio_tone_unlock();
}

static esp_err_t audio_measure_level_locked(uint32_t duration_ms,
                                            solar_os_audio_level_t *level)
{
    if (duration_ms == 0 || duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS || level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(level, 0, sizeof(*level));
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = ESP_OK;

    int16_t samples[AUDIO_LEVEL_BUFFER_SAMPLES];
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint64_t sum_abs = 0;
    uint32_t peak = 0;

    while (esp_timer_get_time() < deadline_us) {
        ret = solar_os_board_audio_read(samples, sizeof(samples));
        if (ret != ESP_OK) {
            return ret;
        }

        for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
            const uint32_t value = audio_abs_i16(samples[i]);
            if (value > peak) {
                peak = value;
            }
            sum_abs += value;
        }
        level->samples += sizeof(samples) / sizeof(samples[0]);
    }

    if (level->samples == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t average = (uint32_t)(sum_abs / level->samples);
    level->peak_percent = clamp_percent_u32((peak * 100U) / 32767U);
    level->average_percent = clamp_percent_u32((average * 100U) / 32767U);
    SOLAR_OS_LOGI(TAG,
             "level: samples=%" PRIu32 " peak=%u avg=%u",
             level->samples,
             level->peak_percent,
             level->average_percent);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_measure_level(uint32_t duration_ms, solar_os_audio_level_t *level)
{
    if (!solar_os_audio_backend_has_input()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = audio_operation_take(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_input_stream.active ? ESP_ERR_INVALID_STATE :
        audio_measure_level_locked(duration_ms, level);
    audio_operation_give();
    return ret;
}

static esp_err_t audio_measure_channel_level_locked(uint8_t channel,
                                                    uint32_t duration_ms,
                                                    solar_os_audio_level_t *level)
{
    if (duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        level == NULL ||
        channel >= SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(level, 0, sizeof(*level));
#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = ESP_OK;

    int16_t samples[AUDIO_LEVEL_BUFFER_SAMPLES];
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    uint64_t sum_abs = 0;
    uint32_t peak = 0;

    while (esp_timer_get_time() < deadline_us) {
        ret = solar_os_board_audio_read(samples, sizeof(samples));
        if (ret != ESP_OK) {
            return ret;
        }

        for (size_t i = channel;
             i < sizeof(samples) / sizeof(samples[0]);
             i += SOLAR_OS_AUDIO_BACKEND_DEFAULT_CHANNELS) {
            const uint32_t value = audio_abs_i16(samples[i]);
            if (value > peak) {
                peak = value;
            }
            sum_abs += value;
            level->samples++;
        }
    }

    if (level->samples == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint32_t average = (uint32_t)(sum_abs / level->samples);
    level->peak_percent = clamp_percent_u32((peak * 100U) / 32767U);
    level->average_percent = clamp_percent_u32((average * 100U) / 32767U);
    SOLAR_OS_LOGD(TAG,
             "level ch%u: samples=%" PRIu32 " peak=%u avg=%u",
             (unsigned)channel,
             level->samples,
             level->peak_percent,
             level->average_percent);
    return ESP_OK;
#endif
}

esp_err_t solar_os_audio_measure_channel_level(uint8_t channel,
                                               uint32_t duration_ms,
                                               solar_os_audio_level_t *level)
{
    if (!solar_os_audio_backend_has_input()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = audio_operation_take(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = audio_input_stream.active ? ESP_ERR_INVALID_STATE :
        audio_measure_channel_level_locked(channel, duration_ms, level);
    audio_operation_give();
    return ret;
}

static esp_err_t audio_loopback_locked(uint32_t duration_ms, uint8_t volume)
{
    if (duration_ms == 0 ||
        duration_ms > SOLAR_OS_AUDIO_TEST_MAX_MS ||
        !audio_volume_arg_valid(volume)) {
        return ESP_ERR_INVALID_ARG;
    }

#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    return ESP_ERR_NOT_SUPPORTED;
#else
    esp_err_t ret = audio_apply_playback_volume(volume);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t *buffer = solar_os_memory_alloc(AUDIO_LOOPBACK_BUFFER_BYTES,
                                             SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                             "audio.loopback");
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)duration_ms * 1000);
    while (esp_timer_get_time() < deadline_us) {
        ret = solar_os_board_audio_read(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
        if (ret != ESP_OK) {
            break;
        }
        ret = solar_os_board_audio_write(buffer, AUDIO_LOOPBACK_BUFFER_BYTES);
        if (ret != ESP_OK) {
            break;
        }
    }

    solar_os_memory_free(buffer);
    SOLAR_OS_LOGI(TAG, "loopback: %" PRIu32 " ms vol=%u ret=%s",
             duration_ms,
             volume,
             esp_err_to_name(ret));
    return ret;
#endif
}

esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    if (!solar_os_audio_backend_has_input()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    esp_err_t ret = audio_operation_take(portMAX_DELAY);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = (audio_output_stream.active || audio_input_stream.active) ?
        ESP_ERR_INVALID_STATE : audio_loopback_locked(duration_ms, volume);
    audio_operation_give();
    return ret;
}
#else
esp_err_t solar_os_audio_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_deinit(void)
{
}

esp_err_t solar_os_audio_set_volume(uint8_t volume)
{
    if (volume > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = audio_set_selected_output_volume(volume);
    if (ret == ESP_OK) {
        audio_global_volume = volume;
        if (volume > 0U) {
            audio_mute_restore_volume = volume;
        }
    }
    return ret;
}

esp_err_t solar_os_audio_toggle_mute(uint8_t *volume_after)
{
    uint8_t target = audio_mute_restore_volume;
    if (audio_global_volume > 0U) {
        audio_mute_restore_volume = audio_global_volume;
        target = 0U;
    } else if (target == 0U || target > 100U) {
        target = AUDIO_GLOBAL_DEFAULT_VOLUME;
    }

    const esp_err_t ret = solar_os_audio_set_volume(target);
    if (volume_after != NULL) {
        *volume_after = ret == ESP_OK ? target : audio_global_volume;
    }
    return ret;
}

esp_err_t solar_os_audio_set_mic_gain(float gain_db)
{
    (void)gain_db;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_stream_open(const char *owner,
                                     uint32_t timeout_ms,
                                     solar_os_audio_stream_t **stream,
                                     solar_os_audio_stream_format_t *format)
{
    (void)owner;
    (void)timeout_ms;
    if (stream != NULL) {
        *stream = NULL;
    }
    if (format != NULL) {
        memset(format, 0, sizeof(*format));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_stream_write(solar_os_audio_stream_t *stream,
                                      const void *data,
                                      size_t len)
{
    (void)stream;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_stream_close(solar_os_audio_stream_t *stream)
{
    (void)stream;
}

esp_err_t solar_os_audio_input_stream_open(
    const char *owner,
    uint32_t timeout_ms,
    solar_os_audio_input_stream_t **stream,
    solar_os_audio_stream_format_t *format)
{
    (void)owner;
    (void)timeout_ms;
    if (stream != NULL) {
        *stream = NULL;
    }
    if (format != NULL) {
        memset(format, 0, sizeof(*format));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_input_stream_read(
    solar_os_audio_input_stream_t *stream,
    void *data,
    size_t len)
{
    (void)stream;
    (void)data;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_input_stream_close(solar_os_audio_input_stream_t *stream)
{
    (void)stream;
}

esp_err_t solar_os_audio_register_streams(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_unregister_streams(const char *id)
{
    (void)id;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_play_tone(uint32_t frequency_hz,
                                   uint32_t duration_ms,
                                   uint8_t volume)
{
    (void)frequency_hz;
    (void)duration_ms;
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_tone_enqueue(const solar_os_audio_tone_request_t *request,
                                      uint32_t *request_id)
{
    (void)request;
    if (request_id != NULL) {
        *request_id = 0U;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_tone_cancel(uint32_t request_id)
{
    (void)request_id;
    return ESP_ERR_NOT_SUPPORTED;
}

void solar_os_audio_tone_queue_get_status(solar_os_audio_tone_queue_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

esp_err_t solar_os_audio_measure_level(uint32_t duration_ms,
                                       solar_os_audio_level_t *level)
{
    (void)duration_ms;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_measure_channel_level(uint8_t channel,
                                               uint32_t duration_ms,
                                               solar_os_audio_level_t *level)
{
    (void)channel;
    (void)duration_ms;
    (void)level;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t solar_os_audio_loopback(uint32_t duration_ms, uint8_t volume)
{
    (void)duration_ms;
    (void)volume;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

esp_err_t solar_os_audio_get_wav_info(const char *path, solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || info == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t ret = audio_wav_read_info_from_file(file, info, NULL);
    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    return ret;
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
esp_err_t solar_os_audio_get_mp3_info(const char *path, solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || info == NULL) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const esp_err_t ret = audio_mp3_probe_from_file(file, info);
    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    return ret;
}
#endif

static bool audio_capture_device_for_stream(
    const char *stream_id,
    solar_os_audio_device_info_t *device)
{
    if (stream_id == NULL || device == NULL) {
        return false;
    }
    const size_t count = solar_os_audio_device_count();
    for (size_t i = 0U; i < count; i++) {
        solar_os_audio_device_info_t candidate;
        if (solar_os_audio_device_get(i, &candidate) &&
            strcmp(candidate.capture_stream, stream_id) == 0) {
            *device = candidate;
            return true;
        }
    }
    return false;
}

static esp_err_t audio_player_convert_block(
    solar_os_audio_player_t *player,
    solar_os_audio_s16_converter_t *converter,
    const int16_t *source_samples,
    size_t source_frames,
    const solar_os_stream_audio_format_t *source_format,
    const solar_os_stream_audio_format_t *sink_format,
    int16_t *converted,
    size_t converted_capacity)
{
    bool source_done = false;
    do {
        size_t output_samples = 0U;
        esp_err_t err = solar_os_audio_s16_convert(
            converter, source_samples, source_frames, source_format,
            sink_format, converted, converted_capacity,
            &output_samples, &source_done);
        if (err != ESP_OK) {
            return err;
        }
        if (output_samples == 0U) {
            continue;
        }
        err = solar_os_audio_player_write(
            player, converted, output_samples * sizeof(converted[0]), NULL);
        if (err != ESP_OK) {
            return err;
        }
    } while (!source_done);
    return ESP_OK;
}

esp_err_t solar_os_audio_monitor_stream(
    uint8_t volume,
    const solar_os_audio_wav_options_t *options,
    solar_os_audio_wav_info_t *info)
{
    if (!audio_volume_arg_valid(volume)) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *owner = options != NULL && options->owner != NULL ?
        options->owner : "monitor";
    const solar_os_stream_open_options_t source_options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = UINT32_MAX,
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
    };
    solar_os_stream_handle_t source = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_audio_device_info_t source_device = {0};
    const char *capture_stream = options != NULL ? options->capture_stream : NULL;
    SOLAR_OS_LOGI(TAG, "monitor opening capture=%s owner=%s volume=%u",
                  capture_stream != NULL && capture_stream[0] != '\0' ?
                      capture_stream : "default",
                  owner, (unsigned)volume);
    esp_err_t ret;
    if (capture_stream != NULL && capture_stream[0] != '\0') {
        ret = solar_os_stream_open_ex(capture_stream, owner, &source_options,
                                      &source);
        (void)audio_capture_device_for_stream(capture_stream, &source_device);
    } else {
        ret = solar_os_audio_open_default(
            SOLAR_OS_STREAM_DIRECTION_SOURCE, owner, &source_options,
            &source, &source_device);
    }
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "monitor capture open failed stream=%s ret=%s",
                      capture_stream != NULL && capture_stream[0] != '\0' ?
                          capture_stream : "default",
                      esp_err_to_name(ret));
        return ret;
    }
    SOLAR_OS_LOGI(TAG,
                  "monitor capture opened stream=%s format=%" PRIu32
                  "Hz/%uch/%ubit",
                  source.id, source.audio.sample_rate,
                  (unsigned)source.audio.channels,
                  (unsigned)source.audio.bits_per_sample);
    if (source.audio.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        source.audio.bits_per_sample != 16U || source.audio.sample_rate == 0U ||
        source.audio.channels == 0U || source.audio.channels > 2U) {
        solar_os_stream_close(&source);
        return ESP_ERR_NOT_SUPPORTED;
    }

    solar_os_audio_player_t *player = NULL;
    solar_os_stream_audio_format_t sink_format;
    solar_os_audio_device_info_t sink_device;
    const solar_os_audio_player_options_t player_options = {
        .owner = owner,
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
        .volume = volume,
        .buffered = true,
        .external_buffer_bytes = 8U * 1024U,
        .internal_buffer_bytes = 8U * 1024U,
        .target_ms = 40U,
    };
    ret = solar_os_audio_player_create(
        &player_options, &player, &sink_format, &sink_device);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "monitor playback open failed ret=%s",
                      esp_err_to_name(ret));
        solar_os_stream_close(&source);
        return ret;
    }
    SOLAR_OS_LOGI(TAG,
                  "monitor playback opened device=%s stream=%s format=%" PRIu32
                  "Hz/%uch/%ubit",
                  sink_device.id, sink_device.playback_stream,
                  sink_format.sample_rate, (unsigned)sink_format.channels,
                  (unsigned)sink_format.bits_per_sample);
    if (options != NULL && options->device != NULL) {
        options->device(&sink_device, options->user);
    }

    uint8_t *source_buffer = solar_os_memory_alloc(
        AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.monitor.source");
    int16_t *sink_buffer = solar_os_memory_alloc(
        AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.monitor.sink");
    if (source_buffer == NULL || sink_buffer == NULL) {
        ret = ESP_ERR_NO_MEM;
    }

    const size_t source_frame_bytes =
        source.audio.channels * sizeof(int16_t);
    size_t read_bytes = AUDIO_WAV_BUFFER_BYTES -
        (AUDIO_WAV_BUFFER_BYTES % source_frame_bytes);
    const size_t provider_bytes =
        source.audio.frames_per_block * source_frame_bytes;
    if (provider_bytes > 0U && provider_bytes <= read_bytes) {
        read_bytes = provider_bytes;
    }
    solar_os_audio_s16_converter_t converter = {0};
    solar_os_audio_wav_info_t current = {
        .sample_rate = source.audio.sample_rate,
        .channels = source.audio.channels,
        .bits_per_sample = 16U,
        .block_align = (uint16_t)source_frame_bytes,
    };
    uint64_t total_frames = 0U;
    const uint32_t progress_interval_ms =
        audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() +
        ((int64_t)progress_interval_ms * 1000);
    bool cancelled = false;

    while (ret == ESP_OK) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        size_t read_len = 0U;
        ret = solar_os_stream_read(&source, source_buffer, read_bytes, 0U,
                                   &read_len);
        if (ret != ESP_OK || read_len == 0U ||
            (read_len % source_frame_bytes) != 0U) {
            ret = ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
            break;
        }
        const size_t source_frames = read_len / source_frame_bytes;
        if (options != NULL && options->samples != NULL) {
            options->samples((const int16_t *)source_buffer,
                             source_frames * source.audio.channels,
                             source.audio.channels, options->user);
        }
        ret = audio_player_convert_block(
            player, &converter, (const int16_t *)source_buffer, source_frames,
            &source.audio, &sink_format, sink_buffer,
            AUDIO_WAV_BUFFER_BYTES / sizeof(sink_buffer[0]));
        if (ret != ESP_OK) {
            break;
        }
        total_frames += source_frames;
        const uint64_t total_bytes = total_frames * source_frame_bytes;
        current.data_bytes = total_bytes > UINT32_MAX ?
            UINT32_MAX : (uint32_t)total_bytes;
        current.duration_ms = (uint32_t)(
            (total_frames * 1000U) / source.audio.sample_rate);
        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_progress_us) {
            audio_wav_report_progress(options, &current, false, false);
            next_progress_us = now_us +
                ((int64_t)progress_interval_ms * 1000);
        }
    }

    if (ret == ESP_OK) {
        ret = solar_os_audio_player_finish(player, NULL);
    }
    solar_os_audio_player_destroy(player);
    solar_os_stream_close(&source);
    solar_os_memory_free(source_buffer);
    solar_os_memory_free(sink_buffer);
    if (info != NULL) {
        *info = current;
    }
    audio_wav_report_progress(options, &current, true, cancelled);
    SOLAR_OS_LOGI(TAG,
                  "monitor stream: frames=%" PRIu64 " ms=%" PRIu32 " ret=%s",
                  total_frames, current.duration_ms, esp_err_to_name(ret));
    return ret;
}

static esp_err_t audio_record_write_block(
    FILE *file,
    solar_os_audio_wav_info_t *current,
    const solar_os_audio_wav_options_t *options,
    int16_t *samples,
    size_t sample_count)
{
    if (options != NULL && options->samples != NULL) {
        options->samples(samples, sample_count, current->channels,
                         options->user);
    }
    if (audio_wav_should_pause(options)) {
        return ESP_OK;
    }
    const size_t bytes_per_sample = current->bits_per_sample / 8U;
    const size_t remaining_samples =
        (UINT32_MAX - AUDIO_WAV_HEADER_BYTES - current->data_bytes) /
        bytes_per_sample;
    if (sample_count > remaining_samples) {
        sample_count = remaining_samples;
    }
    if (current->bits_per_sample == 8U) {
        uint8_t *encoded = (uint8_t *)samples;
        for (size_t i = 0U; i < sample_count; i++) {
            encoded[i] = (uint8_t)(((int32_t)samples[i] >> 8) + 128);
        }
    }
    const size_t bytes = sample_count * bytes_per_sample;
    const esp_err_t err = audio_wav_write_exact(file, samples, bytes);
    if (err == ESP_OK) {
        current->data_bytes += (uint32_t)bytes;
        current->duration_ms = (uint32_t)(
            (((uint64_t)current->data_bytes / current->block_align) * 1000U) /
            current->sample_rate);
    }
    return err;
}

static esp_err_t audio_record_wav_stream(const char *path,
                                         uint32_t duration_ms,
                                         const solar_os_audio_wav_options_t *options,
                                         solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' ||
        duration_ms > SOLAR_OS_AUDIO_WAV_MAX_MS) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *owner = options != NULL && options->owner != NULL ?
        options->owner : "arecord";
    const char *capture_stream = options != NULL ? options->capture_stream : NULL;
    SOLAR_OS_LOGI(TAG,
                  "record opening path=%s capture=%s owner=%s requested=%" PRIu32
                  "Hz/%uch/%ubit",
                  path,
                  capture_stream != NULL && capture_stream[0] != '\0' ?
                      capture_stream : "default",
                  owner,
                  options != NULL ? options->record_format.sample_rate : 0U,
                  (unsigned)(options != NULL ?
                      options->record_format.channels : 0U),
                  (unsigned)(options != NULL ?
                      options->record_format.bits_per_sample : 0U));
    esp_err_t ret = ESP_OK;
    const bool monitor_available = options != NULL &&
        (options->monitor || options->should_monitor != NULL);

    solar_os_stream_handle_t stream = SOLAR_OS_STREAM_HANDLE_INIT;
    const solar_os_stream_open_options_t open_options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .timeout_ms = UINT32_MAX,
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
    };
    solar_os_audio_device_info_t capture_device = {0};
    if (capture_stream != NULL && capture_stream[0] != '\0') {
        ret = solar_os_stream_open_ex(capture_stream, owner, &open_options,
                                      &stream);
        (void)audio_capture_device_for_stream(capture_stream, &capture_device);
    } else {
        ret = solar_os_audio_open_default(
            SOLAR_OS_STREAM_DIRECTION_SOURCE, owner, &open_options,
            &stream, &capture_device);
    }
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "record capture open failed stream=%s ret=%s",
                      capture_stream != NULL && capture_stream[0] != '\0' ?
                          capture_stream : "default",
                      esp_err_to_name(ret));
        return ret;
    }
    SOLAR_OS_LOGI(TAG,
                  "record capture opened stream=%s format=%" PRIu32
                  "Hz/%uch/%ubit",
                  stream.id, stream.audio.sample_rate,
                  (unsigned)stream.audio.channels,
                  (unsigned)stream.audio.bits_per_sample);
    if (stream.audio.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
        stream.audio.bits_per_sample != 16U || stream.audio.sample_rate == 0U ||
        stream.audio.channels == 0U || stream.audio.channels > 2U) {
        solar_os_stream_close(&stream);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (options != NULL && options->device != NULL &&
        capture_device.id[0] != '\0') {
        options->device(&capture_device, options->user);
    }

    solar_os_audio_wav_info_t current = {
        .sample_rate = options != NULL &&
            options->record_format.sample_rate != 0U ?
            options->record_format.sample_rate : stream.audio.sample_rate,
        .channels = options != NULL && options->record_format.channels != 0U ?
            options->record_format.channels : stream.audio.channels,
        .bits_per_sample = options != NULL &&
            options->record_format.bits_per_sample != 0U ?
            options->record_format.bits_per_sample : 16U,
    };
    if (current.sample_rate < 4000U || current.sample_rate > 192000U ||
        current.channels == 0U || current.channels > 2U ||
        (current.bits_per_sample != 8U && current.bits_per_sample != 16U)) {
        solar_os_stream_close(&stream);
        return ESP_ERR_NOT_SUPPORTED;
    }
    current.block_align = (uint16_t)(
        current.channels * (current.bits_per_sample / 8U));
    const uint32_t frame_bytes = current.block_align;
    uint32_t target_bytes =
        (UINT32_MAX - AUDIO_WAV_HEADER_BYTES) / frame_bytes * frame_bytes;
    if (duration_ms != 0U) {
        const uint64_t target_frames =
            ((uint64_t)current.sample_rate * duration_ms) / 1000U;
        const uint64_t target_bytes64 = target_frames * frame_bytes;
        if (target_bytes64 > UINT32_MAX - AUDIO_WAV_HEADER_BYTES) {
            solar_os_stream_close(&stream);
            return ESP_ERR_INVALID_SIZE;
        }
        target_bytes = (uint32_t)target_bytes64;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        SOLAR_OS_LOGW(TAG, "record file open failed path=%s errno=%d",
                      path, errno);
        solar_os_stream_close(&stream);
        return ESP_FAIL;
    }
    uint8_t header[AUDIO_WAV_HEADER_BYTES];
    audio_wav_build_header(header, &current);
    ret = audio_wav_write_exact(file, header, sizeof(header));

    uint8_t *source_buffer = NULL;
    int16_t *record_buffer = NULL;
    int16_t *monitor_buffer = NULL;
    if (ret == ESP_OK) {
        source_buffer = solar_os_memory_alloc(
            AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "audio.wav.source");
        record_buffer = solar_os_memory_alloc(
            AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
            "audio.wav.record");
        if (monitor_available) {
            monitor_buffer = solar_os_memory_alloc(
                AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                "audio.wav.monitor");
        }
        if (source_buffer == NULL || record_buffer == NULL ||
            (monitor_available && monitor_buffer == NULL)) {
            ret = ESP_ERR_NO_MEM;
        }
    }

    const solar_os_stream_audio_format_t record_format = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = current.sample_rate,
        .channels = current.channels,
        .bits_per_sample = 16U,
    };
    solar_os_audio_s16_converter_t record_converter = {0};
    solar_os_audio_s16_converter_t monitor_converter = {0};
    solar_os_audio_player_t *monitor_player = NULL;
    solar_os_stream_audio_format_t monitor_format = {0};
    const size_t source_frame_bytes =
        stream.audio.channels * sizeof(int16_t);
    size_t read_bytes = AUDIO_WAV_BUFFER_BYTES -
        (AUDIO_WAV_BUFFER_BYTES % source_frame_bytes);
    const size_t provider_bytes =
        stream.audio.frames_per_block * source_frame_bytes;
    if (provider_bytes > 0U && provider_bytes <= read_bytes) {
        read_bytes = provider_bytes;
    }
    const size_t converted_capacity =
        AUDIO_WAV_BUFFER_BYTES / sizeof(int16_t);
    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() +
        ((int64_t)progress_interval_ms * 1000);
    bool cancelled = false;

    while (ret == ESP_OK && current.data_bytes < target_bytes) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }
        const bool monitor_enabled = audio_wav_should_monitor(options);
        if (monitor_enabled && monitor_player == NULL) {
            solar_os_audio_device_info_t monitor_device = {0};
            const solar_os_audio_player_options_t player_options = {
                .owner = owner,
                .requested_audio = {
                    .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
                    .bits_per_sample = 16U,
                },
                .volume = options->monitor_volume,
                .buffered = true,
                .external_buffer_bytes = 8U * 1024U,
                .internal_buffer_bytes = 8U * 1024U,
                .target_ms = 40U,
            };
            ret = solar_os_audio_player_create(
                &player_options, &monitor_player, &monitor_format,
                &monitor_device);
            if (ret != ESP_OK) {
                SOLAR_OS_LOGW(TAG, "record monitor start failed ret=%s",
                              esp_err_to_name(ret));
                break;
            }
            memset(&monitor_converter, 0, sizeof(monitor_converter));
            SOLAR_OS_LOGI(TAG,
                          "record monitor on device=%s stream=%s format=%" PRIu32
                          "Hz/%uch/%ubit volume=%u",
                          monitor_device.id, monitor_device.playback_stream,
                          monitor_format.sample_rate,
                          (unsigned)monitor_format.channels,
                          (unsigned)monitor_format.bits_per_sample,
                          (unsigned)options->monitor_volume);
            if (options->device != NULL) {
                options->device(&monitor_device, options->user);
            }
        } else if (!monitor_enabled && monitor_player != NULL) {
            solar_os_audio_player_destroy(monitor_player);
            monitor_player = NULL;
            memset(&monitor_converter, 0, sizeof(monitor_converter));
            SOLAR_OS_LOGI(TAG, "record monitor off");
        }
        size_t read_len = 0U;
        ret = solar_os_stream_read(&stream, source_buffer, read_bytes, 0U,
                                   &read_len);
        if (ret != ESP_OK || read_len == 0U ||
            (read_len % source_frame_bytes) != 0U) {
            ret = ret != ESP_OK ? ret : ESP_ERR_INVALID_SIZE;
            break;
        }
        const size_t source_frames = read_len / source_frame_bytes;
        if (monitor_player != NULL) {
            ret = audio_player_convert_block(
                monitor_player, &monitor_converter,
                (const int16_t *)source_buffer, source_frames,
                &stream.audio, &monitor_format, monitor_buffer,
                converted_capacity);
            if (ret != ESP_OK) {
                break;
            }
        }

        bool source_done = false;
        do {
            size_t output_samples = 0U;
            ret = solar_os_audio_s16_convert(
                &record_converter, (const int16_t *)source_buffer,
                source_frames, &stream.audio, &record_format,
                record_buffer, converted_capacity, &output_samples,
                &source_done);
            if (ret != ESP_OK) {
                break;
            }
            const size_t bytes_per_sample = current.bits_per_sample / 8U;
            size_t wanted_samples =
                (target_bytes - current.data_bytes) / bytes_per_sample;
            if (output_samples > wanted_samples) {
                output_samples = wanted_samples;
            }
            ret = audio_record_write_block(
                file, &current, options, record_buffer, output_samples);
        } while (ret == ESP_OK && !source_done &&
                 current.data_bytes < target_bytes);

        const int64_t now_us = esp_timer_get_time();
        if (ret == ESP_OK && now_us >= next_progress_us) {
            audio_wav_report_progress(options, &current, false, false);
            next_progress_us = now_us +
                ((int64_t)progress_interval_ms * 1000);
        }
    }

    audio_wav_build_header(header, &current);
    if (fseek(file, 0, SEEK_SET) != 0 ||
        audio_wav_write_exact(file, header, sizeof(header)) != ESP_OK ||
        fflush(file) != 0) {
        if (ret == ESP_OK || ret == ESP_ERR_TIMEOUT) {
            ret = ESP_FAIL;
        }
    }
    const int close_errno = errno;
    if (fclose(file) != 0 && (ret == ESP_OK || ret == ESP_ERR_TIMEOUT)) {
        ret = ESP_FAIL;
    }
    errno = close_errno;
    if (monitor_player != NULL) {
        if (ret == ESP_OK) {
            ret = solar_os_audio_player_finish(monitor_player, NULL);
        }
        solar_os_audio_player_destroy(monitor_player);
    }
    solar_os_memory_free(source_buffer);
    solar_os_memory_free(record_buffer);
    solar_os_memory_free(monitor_buffer);
    solar_os_stream_close(&stream);

    if (info != NULL) {
        *info = current;
    }
    audio_wav_report_progress(options, &current, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "record wav %s: bytes=%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path, current.data_bytes, current.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_record_wav(const char *path,
                                    uint32_t duration_ms,
                                    const solar_os_audio_wav_options_t *options,
                                    solar_os_audio_wav_info_t *info)
{
    return audio_record_wav_stream(path, duration_ms, options, info);
}

static bool audio_playback_use_buffered_player(void)
{
    /* Without external stacks, buffering would require two internal workers. */
    solar_os_task_admission_status_t status;
    solar_os_task_get_admission_status(&status);
    return status.external_stacks_supported;
}

static esp_err_t audio_play_wav_stream(const char *path,
                                       uint8_t volume,
                                       const solar_os_audio_wav_options_t *options,
                                       solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || !audio_volume_arg_valid(volume)) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    solar_os_audio_wav_info_t source;
    long data_offset = 0;
    esp_err_t ret = audio_wav_read_info_from_file(file, &source, &data_offset);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }
    if (source.data_bytes == 0U ||
        (source.bits_per_sample != 8U && source.bits_per_sample != 16U)) {
        fclose(file);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (fseek(file, data_offset, SEEK_SET) != 0) {
        fclose(file);
        return ESP_FAIL;
    }

    solar_os_audio_player_t *player = NULL;
    const solar_os_audio_player_options_t player_options = {
        .owner = options != NULL && options->owner != NULL ?
            options->owner : "aplay",
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
        .volume = volume,
        .buffered = audio_playback_use_buffered_player(),
        .external_buffer_bytes = SOLAR_OS_AUDIO_PLAYER_DEFAULT_BUFFER_BYTES,
        .internal_buffer_bytes = SOLAR_OS_AUDIO_PLAYER_DEFAULT_BUFFER_BYTES,
        .target_ms = SOLAR_OS_AUDIO_PLAYER_DEFAULT_TARGET_MS,
        .should_cancel = options != NULL ? options->should_cancel : NULL,
        .cancel_user = options != NULL ? options->user : NULL,
        .should_pause = options != NULL ? options->should_pause : NULL,
        .pause_user = options != NULL ? options->user : NULL,
        .samples = options != NULL ? options->samples : NULL,
        .user = options != NULL ? options->user : NULL,
    };
    solar_os_stream_audio_format_t output_format;
    solar_os_audio_device_info_t device;
    ret = solar_os_audio_player_create(
        &player_options, &player, &output_format, &device);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }
    if (options != NULL && options->device != NULL) {
        options->device(&device, options->user);
    }

    uint8_t *buffer = solar_os_memory_alloc(
        AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.wav.source");
    int16_t *output = solar_os_memory_alloc(
        AUDIO_WAV_BUFFER_BYTES, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.wav.output");
    if (buffer == NULL || output == NULL) {
        fclose(file);
        solar_os_memory_free(buffer);
        solar_os_memory_free(output);
        solar_os_audio_player_destroy(player);
        return ESP_ERR_NO_MEM;
    }

    const solar_os_stream_audio_format_t source_format = {
        .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
        .sample_rate = source.sample_rate,
        .channels = source.channels,
        .bits_per_sample = 16U,
    };
    solar_os_audio_s16_converter_t converter = {0};
    SOLAR_OS_LOGI(TAG,
                  "play wav opened source=%" PRIu32 "Hz/%uch/%ubit "
                  "output=%" PRIu32 "Hz/%uch/%ubit",
                  source.sample_rate, (unsigned)source.channels,
                  (unsigned)source.bits_per_sample,
                  output_format.sample_rate, (unsigned)output_format.channels,
                  (unsigned)output_format.bits_per_sample);

    solar_os_audio_wav_info_t progress = source;
    progress.data_bytes = 0;
    progress.duration_ms = 0;
    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() + ((int64_t)progress_interval_ms * 1000);
    bool cancelled = false;

    while (progress.data_bytes < source.data_bytes) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        uint32_t chunk = source.data_bytes - progress.data_bytes;
        const uint32_t buffer_limit = source.bits_per_sample == 8U ?
            AUDIO_WAV_BUFFER_BYTES / 2U : AUDIO_WAV_BUFFER_BYTES;
        if (chunk > buffer_limit) {
            chunk = buffer_limit;
        }
        chunk -= chunk % source.block_align;
        if (chunk == 0) {
            break;
        }

        ret = audio_wav_read_exact(file, buffer, chunk);
        if (ret != ESP_OK) {
            break;
        }
        size_t playback_bytes = chunk;
        if (source.bits_per_sample == 8U) {
            int16_t *expanded = (int16_t *)buffer;
            for (size_t i = chunk; i > 0U; i--) {
                expanded[i - 1U] =
                    (int16_t)(((int32_t)buffer[i - 1U] - 128) << 8);
            }
            playback_bytes = chunk * sizeof(int16_t);
        }
        const size_t source_frames =
            playback_bytes / (source.channels * sizeof(int16_t));
        ret = audio_player_convert_block(
            player, &converter, (const int16_t *)buffer, source_frames,
            &source_format, &output_format, output,
            AUDIO_WAV_BUFFER_BYTES / sizeof(output[0]));
        if (ret != ESP_OK) {
            break;
        }

        progress.data_bytes += chunk;
        progress.duration_ms =
            (uint32_t)((((uint64_t)progress.data_bytes / progress.block_align) * 1000U) /
                       progress.sample_rate);

        const int64_t now_us = esp_timer_get_time();
        if (now_us >= next_progress_us) {
            audio_wav_report_progress(options, &progress, false, false);
            next_progress_us = now_us + ((int64_t)progress_interval_ms * 1000);
        }
    }

    if (ret == ESP_OK) {
        ret = solar_os_audio_player_finish(player, NULL);
    }
    solar_os_audio_player_destroy(player);

    const int close_errno = errno;
    fclose(file);
    errno = close_errno;
    solar_os_memory_free(buffer);
    solar_os_memory_free(output);

    if (info != NULL) {
        *info = progress;
    }
    audio_wav_report_progress(options, &progress, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "play wav %s: bytes=%" PRIu32 "/%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path,
             progress.data_bytes,
             source.data_bytes,
             progress.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_play_wav(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    return audio_play_wav_stream(path, volume, options, info);
}

#if SOLAR_OS_PACKAGE_SERVICE_AUDIO_CODECS
static esp_err_t audio_play_mp3_stream(const char *path,
                                       uint8_t volume,
                                       const solar_os_audio_wav_options_t *options,
                                       solar_os_audio_wav_info_t *info)
{
    if (path == NULL || path[0] == '\0' || !audio_volume_arg_valid(volume)) {
        errno = EINVAL;
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = audio_mp3_seek_payload(file);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }

    solar_os_audio_player_t *player = NULL;
    solar_os_stream_audio_format_t output_format;
    const solar_os_audio_player_options_t player_options = {
        .owner = options != NULL && options->owner != NULL ?
            options->owner : "aplay",
        .requested_audio = {
            .sample_format = SOLAR_OS_STREAM_AUDIO_S16_LE,
            .bits_per_sample = 16U,
        },
        .volume = volume,
        .buffered = audio_playback_use_buffered_player(),
        .external_buffer_bytes = SOLAR_OS_AUDIO_PLAYER_DEFAULT_BUFFER_BYTES,
        .internal_buffer_bytes = SOLAR_OS_AUDIO_PLAYER_DEFAULT_BUFFER_BYTES,
        .target_ms = SOLAR_OS_AUDIO_PLAYER_DEFAULT_TARGET_MS,
        .should_cancel = options != NULL ? options->should_cancel : NULL,
        .cancel_user = options != NULL ? options->user : NULL,
        .should_pause = options != NULL ? options->should_pause : NULL,
        .pause_user = options != NULL ? options->user : NULL,
        .samples = options != NULL ? options->samples : NULL,
        .user = options != NULL ? options->user : NULL,
    };
    solar_os_audio_device_info_t device;
    ret = solar_os_audio_player_create(
        &player_options, &player, &output_format, &device);
    if (ret != ESP_OK) {
        fclose(file);
        return ret;
    }
    if (options != NULL && options->device != NULL) {
        options->device(&device, options->user);
    }

    solar_os_audio_mp3_decoder_t *decoder = NULL;
    ret = solar_os_audio_mp3_decoder_create(&decoder);
    uint8_t *input = audio_heap_alloc(AUDIO_MP3_INPUT_BUFFER_BYTES);
    int16_t *decoded = audio_heap_alloc(
        sizeof(*decoded) * SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES);
    int16_t *output = audio_heap_alloc(AUDIO_WAV_BUFFER_BYTES);
    int16_t *playback = audio_heap_alloc(AUDIO_WAV_BUFFER_BYTES);
    if (ret != ESP_OK || input == NULL || decoded == NULL || output == NULL || playback == NULL) {
        if (input == NULL) {
            audio_log_heap_nomem("mp3 input", AUDIO_MP3_INPUT_BUFFER_BYTES);
        }
        if (decoded == NULL) {
            audio_log_heap_nomem(
                "mp3 decoded",
                sizeof(*decoded) * SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES);
        }
        if (output == NULL) {
            audio_log_heap_nomem("mp3 output", AUDIO_WAV_BUFFER_BYTES);
        }
        if (playback == NULL) {
            audio_log_heap_nomem("mp3 playback", AUDIO_WAV_BUFFER_BYTES);
        }
        fclose(file);
        solar_os_audio_mp3_decoder_destroy(decoder);
        solar_os_memory_free(input);
        solar_os_memory_free(decoded);
        solar_os_memory_free(output);
        solar_os_memory_free(playback);
        solar_os_audio_player_destroy(player);
        return ESP_ERR_NO_MEM;
    }

    bool cancelled = false;
    solar_os_audio_wav_info_t progress;
    audio_mp3_fill_output_info(&progress, &output_format, 0U);
    size_t input_len = 0;
    bool eof = false;
    bool decoded_any = false;
    size_t playback_samples = 0;
    solar_os_audio_s16_converter_t converter = {0};
    const uint32_t progress_interval_ms = audio_wav_progress_interval_ms(options);
    int64_t next_progress_us = esp_timer_get_time() + ((int64_t)progress_interval_ms * 1000);

    while (true) {
        if (audio_wav_should_cancel(options)) {
            cancelled = true;
            ret = ESP_ERR_TIMEOUT;
            break;
        }

        ret = audio_mp3_fill_input(file, input, &input_len, &eof);
        if (ret != ESP_OK) {
            break;
        }
        if (input_len == 0 && eof) {
            ret = decoded_any ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
            break;
        }

        solar_os_audio_decoded_frame_t frame;
        size_t consumed = 0U;
        ret = solar_os_audio_mp3_decode(
            decoder, input, input_len, &consumed, decoded,
            SOLAR_OS_AUDIO_MP3_MAX_PCM_SAMPLES, &frame);
        if (ret != ESP_OK) {
            break;
        }
        if (frame.frames > 0U) {
            decoded_any = true;
            bool source_done = false;
            do {
                if (audio_wav_should_cancel(options)) {
                    cancelled = true;
                    ret = ESP_ERR_TIMEOUT;
                    break;
                }

                size_t out_samples = 0U;
                ret = solar_os_audio_s16_convert(
                    &converter,
                    decoded,
                    frame.frames,
                    &frame.format,
                    &output_format,
                    output,
                    AUDIO_MP3_OUTPUT_SAMPLES_MAX,
                    &out_samples,
                    &source_done);
                if (ret != ESP_OK || out_samples == 0U ||
                    (out_samples % output_format.channels) != 0U) {
                    if (ret == ESP_OK) {
                        ret = ESP_ERR_INVALID_RESPONSE;
                    }
                    break;
                }

                const size_t out_bytes = out_samples * sizeof(output[0]);
                ret = audio_mp3_playback_append(player,
                                                &output_format,
                                                playback,
                                                &playback_samples,
                                                output,
                                                out_samples);
                if (ret != ESP_OK) {
                    break;
                }

                progress.data_bytes += (uint32_t)out_bytes;
                progress.duration_ms =
                    (uint32_t)((((uint64_t)progress.data_bytes / progress.block_align) * 1000U) /
                               progress.sample_rate);
            } while (!source_done);

            if (ret != ESP_OK) {
                break;
            }

            const int64_t now_us = esp_timer_get_time();
            if (now_us >= next_progress_us) {
                audio_wav_report_progress(options, &progress, false, false);
                next_progress_us = now_us + ((int64_t)progress_interval_ms * 1000);
            }
        }

        if (consumed == 0) {
            if (!eof && input_len < AUDIO_MP3_INPUT_BUFFER_BYTES) {
                continue;
            }
            consumed = input_len > 0 ? 1U : 0U;
        }
        audio_mp3_consume_input(input, &input_len, consumed);
    }

    if (ret == ESP_OK) {
        ret = audio_mp3_playback_flush(
            player, &output_format, playback, &playback_samples, true);
    }
    if (ret == ESP_OK) {
        ret = solar_os_audio_player_finish(player, NULL);
    }
    solar_os_audio_player_destroy(player);

    {
        const int close_errno = errno;
        fclose(file);
        errno = close_errno;
    }
    solar_os_audio_mp3_decoder_destroy(decoder);
    solar_os_memory_free(input);
    solar_os_memory_free(decoded);
    solar_os_memory_free(output);
    solar_os_memory_free(playback);

    if (info != NULL) {
        *info = progress;
    }
    audio_wav_report_progress(options, &progress, true, cancelled);
    SOLAR_OS_LOGI(TAG,
             "play mp3 %s: bytes=%" PRIu32 " ms=%" PRIu32 " ret=%s",
             path,
             progress.data_bytes,
             progress.duration_ms,
             esp_err_to_name(ret));
    return ret;
}

esp_err_t solar_os_audio_play_mp3(const char *path,
                                  uint8_t volume,
                                  const solar_os_audio_wav_options_t *options,
                                  solar_os_audio_wav_info_t *info)
{
    return audio_play_mp3_stream(path, volume, options, info);
}
#endif

void solar_os_audio_get_status(solar_os_audio_status_t *status)
{
    if (status == NULL) {
        return;
    }

#if !SOLAR_OS_AUDIO_BACKEND_PACKAGE
    *status = (solar_os_audio_status_t){
        .initialized = solar_os_audio_output_available(),
        .sample_rate = 0U,
        .channels = 0U,
        .bits_per_sample = 0U,
        .volume = audio_global_volume,
        .mic_gain_db = 0.0f,
        .i2s_port = -1,
        .mclk_pin = -1,
        .bclk_pin = -1,
        .ws_pin = -1,
        .din_pin = -1,
        .dout_pin = -1,
        .pa_pin = -1,
        .output_codec = "-",
        .input_codec = "-",
    };
#else
    solar_os_board_audio_status_t board_status;
    solar_os_board_audio_get_status(&board_status);

    status->initialized =
        board_status.initialized || solar_os_audio_output_available();
    status->sample_rate = board_status.sample_rate;
    status->channels = board_status.channels;
    status->bits_per_sample = board_status.bits_per_sample;
    status->volume = audio_global_volume;
    status->mic_gain_db = board_status.mic_gain_db;
    status->i2s_port = board_status.i2s_port;
    status->mclk_pin = board_status.mclk_pin;
    status->bclk_pin = board_status.bclk_pin;
    status->ws_pin = board_status.ws_pin;
    status->din_pin = board_status.din_pin;
    status->dout_pin = board_status.dout_pin;
    status->pa_pin = board_status.pa_pin;
    status->output_codec = board_status.output_codec;
    status->input_codec = board_status.input_codec;
#endif

    if (status->sample_rate == 0U) {
        solar_os_audio_device_info_t output;
        if (audio_get_available_output(&output)) {
            status->sample_rate = output.native_format.sample_rate;
            status->channels = output.native_format.channels;
            status->bits_per_sample = output.native_format.bits_per_sample;
        }
    }
}
