#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_SPEECH_SERVICE "speech"
#define SOLAR_OS_NATIVE_SPEECH_ABI 1U
#define SOLAR_OS_NATIVE_SPEECH_WORKER_SERVICE "speech.worker"
#define SOLAR_OS_NATIVE_SPEECH_WORKER_ABI 1U

#define SOLAR_OS_SPEECH_TEXT_MAX 512U
#define SOLAR_OS_SPEECH_QUEUE_CAPACITY 8U
#define SOLAR_OS_SPEECH_RESULT_CAPACITY 8U
#define SOLAR_OS_SPEECH_PITCH_MIN 50U
#define SOLAR_OS_SPEECH_PITCH_MAX 200U
#define SOLAR_OS_SPEECH_PITCH_DEFAULT 100U
#define SOLAR_OS_SPEECH_SPEED_MIN 20U
#define SOLAR_OS_SPEECH_SPEED_MAX 500U
#define SOLAR_OS_SPEECH_SPEED_DEFAULT 100U

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
    /* Zero selects PicoTTS's default value of 100. */
    uint16_t pitch;
    uint16_t speed;
    bool drop_if_busy;
} solar_os_speech_request_t;

typedef struct {
    uint8_t volume;
    /* Zero selects PicoTTS's default value of 100. */
    uint16_t pitch;
    uint16_t speed;
    bool drop_if_busy;
} solar_os_speech_stream_options_t;

typedef struct {
    uint32_t id;
    solar_os_speech_request_state_t state;
    int32_t error;
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

/* Enqueue and write copy the text before returning. A stream accepts one
 * queued chunk at a time, plus the chunk the worker is currently consuming.
 * Retry a full chunk slot after the consumer takes it. A final chunk may be
 * empty. Request status retains a bounded history of completed requests. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*enqueue)(const solar_os_speech_request_t *request, uint32_t *id);
    int (*stream_begin)(const solar_os_speech_stream_options_t *options, uint32_t *id);
    int (*stream_write)(uint32_t id, const char *text, size_t length, bool final);
    int (*cancel)(uint32_t id);
    int (*request_status)(uint32_t id, solar_os_speech_request_status_t *status);
    int (*queue_status)(solar_os_speech_queue_status_t *status);
    const char *(*state_name)(solar_os_speech_request_state_t state);
} solar_os_native_speech_api_v1_t;

typedef struct {
    uint32_t id;
    uint8_t volume;
    uint16_t pitch;
    uint16_t speed;
    bool drop_if_busy;
    bool streaming;
    char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
} solar_os_speech_work_t;

/* A firmware or packaged renderer may claim the single worker. One read
 * can be pending at a time. Stop rejects new work and wakes any read, while
 * keeping the worker claim until close. Close after joining worker tasks.
 * Any unfinished requests are recorded as cancelled when the claim closes. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*open)(solar_os_native_handle_t *worker);
    int (*stop)(solar_os_native_handle_t worker);
    int (*close)(solar_os_native_handle_t worker);
    int (*take)(solar_os_native_handle_t worker, solar_os_speech_work_t *work, uint32_t timeout_ms);
    int (*stream_take)(solar_os_native_handle_t worker, uint32_t id, char *text,
        size_t capacity, size_t *length, bool *final, uint32_t timeout_ms);
    int (*set_state)(solar_os_native_handle_t worker, uint32_t id, solar_os_speech_request_state_t state);
    int (*set_progress)(solar_os_native_handle_t worker, uint32_t id, size_t done, size_t total);
    int (*finish)(solar_os_native_handle_t worker, uint32_t id, solar_os_speech_request_state_t state, int error);
    int (*cancelled)(solar_os_native_handle_t worker, uint32_t id, bool *cancelled);
} solar_os_native_speech_worker_api_v1_t;
