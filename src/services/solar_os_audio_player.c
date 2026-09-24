#include "solar_os_audio_player.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define AUDIO_PLAYER_TASK_STACK 8192U
#define AUDIO_PLAYER_TASK_PRIORITY (tskIDLE_PRIORITY + 3U)
#define AUDIO_PLAYER_BLOCK_BYTES 4096U
#define AUDIO_PLAYER_POLL_MS 20U
#define AUDIO_PLAYER_SINK_TIMEOUT_MS 1000U
#define AUDIO_PLAYER_RAMP_FRAMES 64U
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(AUDIO_PLAYER_TASK_STACK);

struct solar_os_audio_player {
    solar_os_stream_handle_t stream;
    solar_os_audio_device_info_t device;
    solar_os_audio_player_options_t options;
    char owner[SOLAR_OS_AUDIO_STREAM_OWNER_MAX];
    uint8_t *buffer;
    size_t buffer_capacity;
    size_t buffer_read;
    size_t buffer_write;
    size_t buffer_used;
    SemaphoreHandle_t buffer_mutex;
    bool buffer_mutex_external;
    int16_t *sink;
    int16_t *transition;
    int16_t last_frame[2];
    bool fade_in_pending;
    uint32_t underruns;
    TaskHandle_t task;
    volatile bool ready;
    volatile bool done;
    volatile bool stop_requested;
    volatile bool producer_done;
    volatile bool playing;
    volatile esp_err_t error;
    bool stream_opened;
};

static const char *TAG = "solar_os_audio_player";

static void audio_player_set_playing(solar_os_audio_player_t *player,
                                     bool playing);

static bool audio_player_cancelled(const solar_os_audio_player_t *player,
                                   const volatile bool *cancelled)
{
    return (cancelled != NULL && *cancelled) ||
        (player->options.should_cancel != NULL &&
         player->options.should_cancel(player->options.cancel_user));
}

static bool audio_player_paused(const solar_os_audio_player_t *player)
{
    return player->options.should_pause != NULL &&
        player->options.should_pause(player->options.pause_user);
}

static bool audio_player_wait_unpaused(solar_os_audio_player_t *player,
                                       const volatile bool *cancelled)
{
    while (audio_player_paused(player)) {
        audio_player_set_playing(player, false);
        if (player->stop_requested || player->done ||
            audio_player_cancelled(player, cancelled)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
    }
    return true;
}

static size_t audio_player_available(solar_os_audio_player_t *player)
{
    xSemaphoreTake(player->buffer_mutex, portMAX_DELAY);
    const size_t available = player->buffer_used;
    xSemaphoreGive(player->buffer_mutex);
    return available;
}

static size_t audio_player_buffer_write(solar_os_audio_player_t *player,
                                        const uint8_t *data,
                                        size_t len)
{
    xSemaphoreTake(player->buffer_mutex, portMAX_DELAY);
    const size_t space = player->buffer_capacity - player->buffer_used;
    const size_t count = len < space ? len : space;
    const size_t first = count < player->buffer_capacity - player->buffer_write ?
        count : player->buffer_capacity - player->buffer_write;
    memcpy(player->buffer + player->buffer_write, data, first);
    memcpy(player->buffer, data + first, count - first);
    player->buffer_write = (player->buffer_write + count) % player->buffer_capacity;
    player->buffer_used += count;
    xSemaphoreGive(player->buffer_mutex);
    return count;
}

static size_t audio_player_buffer_read(solar_os_audio_player_t *player,
                                       uint8_t *data,
                                       size_t len)
{
    xSemaphoreTake(player->buffer_mutex, portMAX_DELAY);
    const size_t count = len < player->buffer_used ? len : player->buffer_used;
    const size_t first = count < player->buffer_capacity - player->buffer_read ?
        count : player->buffer_capacity - player->buffer_read;
    memcpy(data, player->buffer + player->buffer_read, first);
    memcpy(data + first, player->buffer, count - first);
    player->buffer_read = (player->buffer_read + count) % player->buffer_capacity;
    player->buffer_used -= count;
    xSemaphoreGive(player->buffer_mutex);
    return count;
}

static void audio_player_set_playing(solar_os_audio_player_t *player, bool playing)
{
    if (player->playing == playing) {
        return;
    }
    player->playing = playing;
    if (player->options.state != NULL) {
        player->options.state(playing, player->options.user);
    }
}

static size_t audio_player_target(const solar_os_audio_player_t *player)
{
    const uint64_t bytes_per_second =
        (uint64_t)player->stream.audio.sample_rate *
        player->stream.audio.channels * sizeof(int16_t);
    size_t target = (size_t)((bytes_per_second * player->options.target_ms) / 1000U);
    const size_t maximum = (player->buffer_capacity * 3U) / 4U;
    if (target > maximum) {
        target = maximum;
    }
    if (target < AUDIO_PLAYER_BLOCK_BYTES) {
        target = AUDIO_PLAYER_BLOCK_BYTES;
    }
    return target;
}

static void audio_player_wait_played(const solar_os_audio_player_t *player,
                                     size_t bytes)
{
    const uint64_t bytes_per_second =
        (uint64_t)player->stream.audio.sample_rate *
        player->stream.audio.channels * sizeof(int16_t);
    if (bytes_per_second == 0U || bytes == 0U) {
        return;
    }
    const uint32_t duration_ms = (uint32_t)(
        ((uint64_t)bytes * 1000U + bytes_per_second - 1U) /
        bytes_per_second);
    TickType_t ticks = pdMS_TO_TICKS(duration_ms);
    if (ticks == 0U) {
        ticks = 1U;
    }
    vTaskDelay(ticks);
}

static void audio_player_remember_tail(solar_os_audio_player_t *player,
                                       const int16_t *samples,
                                       size_t bytes)
{
    const uint8_t channels = player->stream.audio.channels;
    const size_t frame_bytes = channels * sizeof(*samples);
    if (samples == NULL || channels == 0U || channels > 2U ||
        bytes < frame_bytes) {
        return;
    }
    const size_t last = bytes / frame_bytes - 1U;
    player->last_frame[0] = samples[last * channels];
    player->last_frame[1] = channels > 1U ?
        samples[last * channels + 1U] : player->last_frame[0];
}

static void audio_player_fade_in(solar_os_audio_player_t *player,
                                 int16_t *samples,
                                 size_t bytes)
{
    if (!player->fade_in_pending || samples == NULL) {
        return;
    }
    const uint8_t channels = player->stream.audio.channels;
    const size_t frame_bytes = channels * sizeof(*samples);
    size_t frames = frame_bytes > 0U ? bytes / frame_bytes : 0U;
    if (frames > AUDIO_PLAYER_RAMP_FRAMES) {
        frames = AUDIO_PLAYER_RAMP_FRAMES;
    }
    for (size_t frame = 0U; frame < frames; frame++) {
        for (uint8_t channel = 0U; channel < channels; channel++) {
            const size_t index = frame * channels + channel;
            samples[index] = (int16_t)(
                ((int32_t)samples[index] * (int32_t)frame) /
                (int32_t)AUDIO_PLAYER_RAMP_FRAMES);
        }
    }
    player->fade_in_pending = false;
}

static esp_err_t audio_player_ramp_to_silence(
    solar_os_audio_player_t *player)
{
    const uint8_t channels = player->stream.audio.channels;
    const size_t frame_bytes = channels * sizeof(player->transition[0]);
    if (player->transition == NULL || channels == 0U || channels > 2U ||
        frame_bytes == 0U) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(player->transition, 0, AUDIO_PLAYER_BLOCK_BYTES);
    size_t frames = AUDIO_PLAYER_BLOCK_BYTES / frame_bytes;
    if (frames > AUDIO_PLAYER_RAMP_FRAMES) {
        frames = AUDIO_PLAYER_RAMP_FRAMES;
    }
    for (size_t frame = 0U; frame < frames; frame++) {
        const int32_t remaining =
            (int32_t)AUDIO_PLAYER_RAMP_FRAMES - (int32_t)frame;
        for (uint8_t channel = 0U; channel < channels; channel++) {
            player->transition[frame * channels + channel] = (int16_t)(
                ((int32_t)player->last_frame[channel] * remaining) /
                (int32_t)AUDIO_PLAYER_RAMP_FRAMES);
        }
    }
    size_t written = 0U;
    const esp_err_t err = solar_os_stream_write(
        &player->stream, player->transition, AUDIO_PLAYER_BLOCK_BYTES,
        AUDIO_PLAYER_SINK_TIMEOUT_MS, &written);
    if (err != ESP_OK || written != AUDIO_PLAYER_BLOCK_BYTES) {
        return err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
    }
    player->last_frame[0] = 0;
    player->last_frame[1] = 0;
    player->fade_in_pending = true;
    return ESP_OK;
}

static esp_err_t audio_player_open(solar_os_audio_player_t *player)
{
    player->stream = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
    const solar_os_stream_open_options_t open_options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SINK,
        .timeout_ms = player->options.open_timeout_ms == 0U ?
            UINT32_MAX : player->options.open_timeout_ms,
        .requested_audio = player->options.requested_audio,
    };
    esp_err_t err = solar_os_audio_open_default(
        SOLAR_OS_STREAM_DIRECTION_SINK,
        player->owner,
        &open_options,
        &player->stream,
        &player->device);
    player->stream_opened = err == ESP_OK;
    if (err == ESP_OK &&
        (player->stream.audio.sample_format != SOLAR_OS_STREAM_AUDIO_S16_LE ||
         player->stream.audio.bits_per_sample != 16U ||
         player->stream.audio.sample_rate == 0U ||
         player->stream.audio.channels == 0U ||
         player->stream.audio.channels > 2U)) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && player->options.volume != SOLAR_OS_AUDIO_VOLUME_GLOBAL) {
        err = solar_os_audio_set_device_volume(player->device.id,
                                               player->options.volume);
    }
    player->error = err;
    return err;
}

static void audio_player_close(solar_os_audio_player_t *player, bool silence)
{
    if (!player->stream_opened) {
        return;
    }
    if (silence) {
        int16_t *tail = player->transition != NULL ?
            player->transition : player->sink;
        memset(tail, 0, AUDIO_PLAYER_BLOCK_BYTES);
        size_t written = 0U;
        const esp_err_t err = solar_os_stream_write(
            &player->stream, tail, AUDIO_PLAYER_BLOCK_BYTES,
            AUDIO_PLAYER_SINK_TIMEOUT_MS, &written);
        if (err == ESP_OK && written > 0U) {
            /* Stream writes may return once data is queued. Keep the sink
             * alive until the zero tail has reached the device before its
             * clocks or amplifier are disabled. */
            audio_player_wait_played(player, written);
        }
    }
    solar_os_stream_close(&player->stream);
    player->stream_opened = false;
    audio_player_set_playing(player, false);
}

static void audio_player_task(void *arg)
{
    solar_os_audio_player_t *player = arg;
    esp_err_t err = audio_player_open(player);
    player->ready = true;

    if (err == ESP_OK) {
        const size_t target = audio_player_target(player);
        const size_t block_align =
            player->stream.audio.channels * sizeof(player->sink[0]);
        size_t provider_block =
            player->stream.audio.frames_per_block * block_align;
        if (provider_block == 0U || provider_block > AUDIO_PLAYER_BLOCK_BYTES) {
            provider_block = block_align;
        }
        size_t filled = 0U;
        bool primed = false;
        while (!player->stop_requested) {
            if (!audio_player_wait_unpaused(player, NULL)) {
                break;
            }
            const size_t available = audio_player_available(player);
            if (!primed) {
                if (available == 0U && player->producer_done) {
                    break;
                }
                if (filled + available < target && !player->producer_done) {
                    vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
                    continue;
                }
                primed = true;
                audio_player_set_playing(player, true);
            }

            const size_t received = audio_player_buffer_read(
                player,
                (uint8_t *)player->sink + filled,
                AUDIO_PLAYER_BLOCK_BYTES - filled);
            filled += received;
            if (filled < AUDIO_PLAYER_BLOCK_BYTES) {
                if (player->producer_done && audio_player_available(player) == 0U) {
                    if (filled == 0U) {
                        break;
                    }
                    size_t write_len =
                        ((filled + provider_block - 1U) / provider_block) * provider_block;
                    if (write_len > AUDIO_PLAYER_BLOCK_BYTES) {
                        write_len = AUDIO_PLAYER_BLOCK_BYTES;
                    }
                    memset((uint8_t *)player->sink + filled, 0, write_len - filled);
                    const size_t played_bytes = filled;
                    audio_player_fade_in(player, player->sink, write_len);
                    size_t written = 0U;
                    err = solar_os_stream_write(
                        &player->stream, player->sink, write_len,
                        AUDIO_PLAYER_SINK_TIMEOUT_MS, &written);
                    if (err != ESP_OK || written != write_len) {
                        player->error = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
                    } else {
                        audio_player_remember_tail(
                            player, player->sink, write_len);
                        if (player->options.samples != NULL) {
                            player->options.samples(
                                player->sink,
                                played_bytes / sizeof(player->sink[0]),
                                player->stream.audio.channels,
                                player->options.user);
                        }
                    }
                    filled = 0U;
                    break;
                }
                if (received == 0U) {
                    err = audio_player_ramp_to_silence(player);
                    if (err != ESP_OK) {
                        player->error = err;
                        break;
                    }
                    player->underruns++;
                    primed = false;
                    audio_player_set_playing(player, false);
                    vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
                }
                continue;
            }

            size_t written = 0U;
            audio_player_fade_in(
                player, player->sink, AUDIO_PLAYER_BLOCK_BYTES);
            err = solar_os_stream_write(&player->stream,
                                        player->sink,
                                        AUDIO_PLAYER_BLOCK_BYTES,
                                        AUDIO_PLAYER_SINK_TIMEOUT_MS,
                                        &written);
            if (err != ESP_OK || written != AUDIO_PLAYER_BLOCK_BYTES) {
                player->error = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
                break;
            }
            audio_player_remember_tail(
                player, player->sink, AUDIO_PLAYER_BLOCK_BYTES);
            if (player->options.samples != NULL) {
                player->options.samples(player->sink,
                                        written / sizeof(player->sink[0]),
                                        player->stream.audio.channels,
                                        player->options.user);
            }
            filled = 0U;
        }

    }
    if (player->underruns > 0U) {
        SOLAR_OS_LOGW(TAG,
                      "buffer underruns: owner=%s count=%u",
                      player->owner,
                      (unsigned)player->underruns);
    }
    audio_player_close(player, true);
    player->done = true;
    solar_os_task_delete_internal(NULL);
}

static bool audio_player_create_buffer(solar_os_audio_player_t *player)
{
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (player->options.external_buffer_bytes > 0U) {
        player->buffer = solar_os_memory_alloc(
            player->options.external_buffer_bytes,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,
            "audio.player.buffer");
        if (player->buffer != NULL) {
            player->buffer_capacity = player->options.external_buffer_bytes;
            player->buffer_mutex = xSemaphoreCreateMutexWithCaps(
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            player->buffer_mutex_external = player->buffer_mutex != NULL;
        }
    }
#endif
    if (player->buffer == NULL) {
        player->buffer = solar_os_memory_alloc(
            player->options.internal_buffer_bytes,
            SOLAR_OS_MEMORY_INTERNAL_PREFERRED,
            "audio.player.buffer");
        if (player->buffer != NULL) {
            player->buffer_capacity = player->options.internal_buffer_bytes;
        }
    }
    if (player->buffer_mutex == NULL) {
        player->buffer_mutex = xSemaphoreCreateMutex();
        player->buffer_mutex_external = false;
    }
    return player->buffer != NULL && player->buffer_mutex != NULL;
}

esp_err_t solar_os_audio_player_create(
    const solar_os_audio_player_options_t *options,
    solar_os_audio_player_t **player_out,
    solar_os_stream_audio_format_t *format,
    solar_os_audio_device_info_t *device)
{
    if (options == NULL || player_out == NULL || options->owner == NULL ||
        options->owner[0] == '\0' ||
        (options->buffered && options->internal_buffer_bytes == 0U) ||
        (options->volume > 100U &&
         options->volume != SOLAR_OS_AUDIO_VOLUME_GLOBAL)) {
        return ESP_ERR_INVALID_ARG;
    }
    *player_out = NULL;
    solar_os_audio_player_t *player = solar_os_memory_alloc(
        sizeof(*player), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "audio.player");
    if (player == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(player, 0, sizeof(*player));
    player->options = *options;
    if (player->options.target_ms == 0U) {
        player->options.target_ms = SOLAR_OS_AUDIO_PLAYER_DEFAULT_TARGET_MS;
    }
    strlcpy(player->owner, options->owner, sizeof(player->owner));
    player->options.owner = player->owner;
    player->error = ESP_OK;
    player->sink = solar_os_memory_alloc(
        AUDIO_PLAYER_BLOCK_BYTES,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.player.sink");
    player->transition = solar_os_memory_alloc(
        AUDIO_PLAYER_BLOCK_BYTES,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "audio.player.transition");
    player->fade_in_pending = true;
    if (player->sink == NULL || player->transition == NULL ||
        (player->options.buffered && !audio_player_create_buffer(player))) {
        solar_os_audio_player_destroy(player);
        return ESP_ERR_NO_MEM;
    }

    if (!player->options.buffered) {
        const esp_err_t err = audio_player_open(player);
        player->ready = true;
        if (err != ESP_OK) {
            solar_os_audio_player_destroy(player);
            return err;
        }
        if (format != NULL) {
            *format = player->stream.audio;
        }
        if (device != NULL) {
            *device = player->device;
        }
        *player_out = player;
        return ESP_OK;
    }

    /* The sink is timing-sensitive and remains active for suspended media
     * apps. Keep its stack internal. Playback is explicitly user-started
     * foreground work for admission purposes. */
    const BaseType_t created = solar_os_task_create_pinned_internal(
        audio_player_task,
        "audio_player",
        AUDIO_PLAYER_TASK_STACK,
        player,
        AUDIO_PLAYER_TASK_PRIORITY,
        &player->task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        solar_os_audio_player_destroy(player);
        return ESP_ERR_NO_MEM;
    }
    while (!player->ready && !player->done) {
        vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
    }
    if (player->error != ESP_OK) {
        const esp_err_t err = player->error;
        solar_os_audio_player_destroy(player);
        return err;
    }
    if (format != NULL) {
        *format = player->stream.audio;
    }
    if (device != NULL) {
        *device = player->device;
    }
    *player_out = player;
    return ESP_OK;
}

esp_err_t solar_os_audio_player_write(solar_os_audio_player_t *player,
                                      const void *data,
                                      size_t len,
                                      const volatile bool *cancelled)
{
    if (player == NULL || data == NULL || len == 0U || player->producer_done) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t block_align =
        player->stream.audio.channels * sizeof(int16_t);
    if (block_align == 0U || (len % block_align) != 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!player->options.buffered) {
        if (!audio_player_wait_unpaused(player, cancelled) ||
            audio_player_cancelled(player, cancelled)) {
            return ESP_ERR_TIMEOUT;
        }
        audio_player_set_playing(player, true);
        size_t written = 0U;
        const esp_err_t err = solar_os_stream_write(
            &player->stream, data, len, AUDIO_PLAYER_SINK_TIMEOUT_MS, &written);
        if (err != ESP_OK || written != len) {
            player->error = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
            return player->error;
        }
        if (player->options.samples != NULL) {
            player->options.samples(data,
                                    written / sizeof(int16_t),
                                    player->stream.audio.channels,
                                    player->options.user);
        }
        return ESP_OK;
    }

    const uint8_t *bytes = data;
    size_t sent = 0U;
    while (sent < len && !player->stop_requested && !player->done) {
        if (!audio_player_wait_unpaused(player, cancelled)) {
            return ESP_ERR_TIMEOUT;
        }
        if (audio_player_cancelled(player, cancelled)) {
            return ESP_ERR_TIMEOUT;
        }
        if (player->error != ESP_OK) {
            return player->error;
        }
        const size_t count = audio_player_buffer_write(player,
                                                       bytes + sent,
                                                       len - sent);
        sent += count;
        if (count == 0U) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
        }
    }
    if (sent != len) {
        return player->error != ESP_OK ? player->error : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t solar_os_audio_player_finish(solar_os_audio_player_t *player,
                                       const volatile bool *cancelled)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    player->producer_done = true;
    if (!player->options.buffered) {
        if (audio_player_cancelled(player, cancelled)) {
            return ESP_ERR_TIMEOUT;
        }
        audio_player_close(player, true);
        player->done = true;
        return player->error;
    }
    while (!player->done) {
        if (audio_player_cancelled(player, cancelled)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
    }
    return player->error;
}

esp_err_t solar_os_audio_player_error(const solar_os_audio_player_t *player)
{
    return player != NULL ? player->error : ESP_ERR_INVALID_ARG;
}

void solar_os_audio_player_destroy(solar_os_audio_player_t *player)
{
    if (player == NULL) {
        return;
    }
    player->stop_requested = true;
    if (player->task != NULL &&
        !solar_os_task_wait_done(player->task,
                                 &player->done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        SOLAR_OS_LOGW(TAG, "player task is slow to stop");
        while (!player->done) {
            vTaskDelay(pdMS_TO_TICKS(AUDIO_PLAYER_POLL_MS));
        }
    }
    if (!player->options.buffered) {
        audio_player_close(player, true);
        player->done = true;
    }
    if (player->buffer_mutex != NULL) {
        if (player->buffer_mutex_external) {
            vSemaphoreDeleteWithCaps(player->buffer_mutex);
        } else {
            vSemaphoreDelete(player->buffer_mutex);
        }
    }
    solar_os_memory_free(player->buffer);
    solar_os_memory_free(player->sink);
    solar_os_memory_free(player->transition);
    solar_os_memory_free(player);
}
