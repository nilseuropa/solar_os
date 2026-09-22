#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SOLAR_OS_SPEECH_TEXT_MAX 512U
#define SOLAR_OS_SPEECH_QUEUE_CAPACITY 8U
#define SOLAR_OS_SPEECH_RESULT_CAPACITY 8U

typedef enum {
    SOLAR_OS_SPEECH_REQUEST_QUEUED,
    SOLAR_OS_SPEECH_REQUEST_WAITING_AUDIO,
    SOLAR_OS_SPEECH_REQUEST_SPEAKING,
    SOLAR_OS_SPEECH_REQUEST_COMPLETE,
    SOLAR_OS_SPEECH_REQUEST_CANCELLED,
    SOLAR_OS_SPEECH_REQUEST_DROPPED,
    SOLAR_OS_SPEECH_REQUEST_FAILED,
} solar_os_speech_request_state_t;

typedef struct {
    const char *text;
    size_t text_len;
    uint8_t volume;
    bool drop_if_busy;
} solar_os_speech_request_t;

typedef struct {
    uint32_t id;
    solar_os_speech_request_state_t state;
    esp_err_t error;
    size_t progress_done;
    size_t progress_total;
} solar_os_speech_request_status_t;

typedef struct {
    bool running;
    size_t queued;
    uint32_t current_id;
    solar_os_speech_request_state_t current_state;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t dropped;
    uint32_t failed;
} solar_os_speech_queue_status_t;

/* Public producer API. Requests are copied before enqueue returns. */
esp_err_t solar_os_speech_enqueue(const solar_os_speech_request_t *request,
                                  uint32_t *request_id);
esp_err_t solar_os_speech_cancel(uint32_t request_id);
bool solar_os_speech_request_status(uint32_t request_id,
                                    solar_os_speech_request_status_t *status);
void solar_os_speech_queue_get_status(solar_os_speech_queue_status_t *status);
const char *solar_os_speech_request_state_name(
    solar_os_speech_request_state_t state);

/* Consumer API owned by the speechd job. */
typedef struct {
    uint32_t id;
    uint8_t volume;
    bool drop_if_busy;
    char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
} solar_os_speech_work_t;

esp_err_t solar_os_speech_worker_start(TaskHandle_t task);
void solar_os_speech_worker_stop(void);
esp_err_t solar_os_speech_worker_take(solar_os_speech_work_t *work,
                                      uint32_t timeout_ms);
esp_err_t solar_os_speech_worker_set_state(uint32_t request_id,
                                           solar_os_speech_request_state_t state);
esp_err_t solar_os_speech_worker_set_progress(uint32_t request_id,
                                              size_t bytes_done,
                                              size_t bytes_total);
esp_err_t solar_os_speech_worker_finish(uint32_t request_id,
                                        solar_os_speech_request_state_t state,
                                        esp_err_t error);
const volatile bool *solar_os_speech_worker_cancel_flag(uint32_t request_id);
