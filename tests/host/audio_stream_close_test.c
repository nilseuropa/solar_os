#include <assert.h>
#include <string.h>

#include "solar_os_audio.h"

#define portMAX_DELAY UINT32_MAX
#define SOLAR_OS_LOGI(tag, ...) ((void)tag)
static const char *TAG = "test";
typedef void *TaskHandle_t;
static TaskHandle_t current_task;
static const char playback_task, capture_task;
static unsigned deinit_calls, lock_takes, lock_gives;
static esp_err_t take_error;

struct solar_os_audio_stream {
    TaskHandle_t task;
    bool active;
    char owner[SOLAR_OS_AUDIO_STREAM_OWNER_MAX];
};
static solar_os_audio_stream_t audio_output_stream;
struct solar_os_audio_input_stream {
    TaskHandle_t task;
    bool active;
    char owner[SOLAR_OS_AUDIO_STREAM_OWNER_MAX];
};
static solar_os_audio_input_stream_t audio_input_stream;

static TaskHandle_t xTaskGetCurrentTaskHandle(void) { return current_task; }
static void audio_board_deinit_if_unused(void) { ++deinit_calls; }
static void audio_operation_give(void) { ++lock_gives; }
static esp_err_t audio_operation_take(uint32_t timeout)
{
    assert(timeout == portMAX_DELAY);
    ++lock_takes;
    return take_error;
}

#include "audio_stream_close.inc"

int main(void)
{
    audio_output_stream = (solar_os_audio_stream_t){ .active = true, .task = (void *)&playback_task, .owner = "play" };
    solar_os_stream_handle_t sink = { .context = &audio_output_stream };
    current_task = (void *)&capture_task;
    assert(audio_playback_stream_close(NULL, &sink) == ESP_ERR_INVALID_STATE);
    assert(sink.context == &audio_output_stream && audio_output_stream.active);
    assert(deinit_calls == 0 && lock_gives == 0);
    current_task = (void *)&playback_task;
    assert(audio_playback_stream_close(NULL, &sink) == ESP_OK && !sink.context);
    assert(!audio_output_stream.active && !audio_output_stream.task && !*audio_output_stream.owner);
    assert(deinit_calls == 1 && lock_gives == 1);

    audio_input_stream = (solar_os_audio_input_stream_t){ .active = true, .task = (void *)&capture_task, .owner = "mic" };
    solar_os_stream_handle_t source = { .context = &audio_input_stream };
    assert(audio_capture_stream_close(NULL, &source) == ESP_ERR_INVALID_STATE && lock_takes == 0);
    current_task = (void *)&capture_task;
    take_error = ESP_ERR_TIMEOUT;
    assert(audio_capture_stream_close(NULL, &source) == ESP_ERR_TIMEOUT);
    assert(source.context == &audio_input_stream && audio_input_stream.active);
    assert(deinit_calls == 1 && lock_takes == 1 && lock_gives == 1);
    take_error = ESP_OK;
    assert(audio_capture_stream_close(NULL, &source) == ESP_OK && !source.context);
    assert(!audio_input_stream.active && !audio_input_stream.task && !*audio_input_stream.owner);
    assert(deinit_calls == 2 && lock_takes == 2 && lock_gives == 2);
    return 0;
}
