#include "solar_os_speech.h"

#include <string.h>

#include "esp_attr.h"
#include "freertos/semphr.h"
#include "solar_os_audio.h"

typedef struct {
    uint32_t id;
    uint8_t volume;
    uint16_t pitch;
    uint16_t speed;
    bool drop_if_busy;
    char text[SOLAR_OS_SPEECH_TEXT_MAX + 1U];
} speech_queue_entry_t;

typedef struct {
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    TaskHandle_t worker;
    bool running;
    speech_queue_entry_t queue[SOLAR_OS_SPEECH_QUEUE_CAPACITY];
    size_t queue_count;
    solar_os_speech_request_status_t results[SOLAR_OS_SPEECH_RESULT_CAPACITY];
    size_t result_count;
    size_t result_next;
    uint32_t next_id;
    uint32_t current_id;
    solar_os_speech_request_state_t current_state;
    size_t current_progress_done;
    size_t current_progress_total;
    volatile bool cancel_current;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t dropped;
    uint32_t failed;
} speech_service_state_t;

static EXT_RAM_BSS_ATTR speech_service_state_t speech;
static portMUX_TYPE speech_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool speech_volume_valid(uint8_t volume)
{
    return volume <= 100U || volume == SOLAR_OS_AUDIO_VOLUME_GLOBAL;
}

static bool speech_pitch_valid(uint16_t pitch)
{
    return pitch == 0U ||
        (pitch >= SOLAR_OS_SPEECH_PITCH_MIN &&
         pitch <= SOLAR_OS_SPEECH_PITCH_MAX);
}

static bool speech_speed_valid(uint16_t speed)
{
    return speed == 0U ||
        (speed >= SOLAR_OS_SPEECH_SPEED_MIN &&
         speed <= SOLAR_OS_SPEECH_SPEED_MAX);
}

static esp_err_t speech_ensure_mutex(void)
{
    portENTER_CRITICAL(&speech_init_lock);
    if (speech.mutex == NULL) {
        speech.mutex = xSemaphoreCreateMutexStatic(&speech.mutex_storage);
        speech.next_id = 1U;
    }
    const bool ready = speech.mutex != NULL;
    portEXIT_CRITICAL(&speech_init_lock);
    return ready ? ESP_OK : ESP_ERR_NO_MEM;
}

static void speech_lock(void)
{
    (void)xSemaphoreTake(speech.mutex, portMAX_DELAY);
}

static void speech_unlock(void)
{
    (void)xSemaphoreGive(speech.mutex);
}

static void speech_store_result_locked(uint32_t id,
                                       solar_os_speech_request_state_t state,
                                       esp_err_t error,
                                       size_t progress_done,
                                       size_t progress_total)
{
    speech.results[speech.result_next] = (solar_os_speech_request_status_t){
        .id = id,
        .state = state,
        .error = error,
        .progress_done = progress_done,
        .progress_total = progress_total,
    };
    speech.result_next =
        (speech.result_next + 1U) % SOLAR_OS_SPEECH_RESULT_CAPACITY;
    if (speech.result_count < SOLAR_OS_SPEECH_RESULT_CAPACITY) {
        speech.result_count++;
    }

    switch (state) {
    case SOLAR_OS_SPEECH_REQUEST_COMPLETE:
        speech.completed++;
        break;
    case SOLAR_OS_SPEECH_REQUEST_CANCELLED:
        speech.cancelled++;
        break;
    case SOLAR_OS_SPEECH_REQUEST_DROPPED:
        speech.dropped++;
        break;
    case SOLAR_OS_SPEECH_REQUEST_FAILED:
        speech.failed++;
        break;
    default:
        break;
    }
}

esp_err_t solar_os_speech_enqueue(const solar_os_speech_request_t *request,
                                  uint32_t *request_id)
{
    if (request_id != NULL) {
        *request_id = 0U;
    }
    if (request == NULL || request->text == NULL || request->text_len == 0U ||
        request->text_len > SOLAR_OS_SPEECH_TEXT_MAX ||
        memchr(request->text, '\0', request->text_len) != NULL ||
        !speech_volume_valid(request->volume) ||
        !speech_pitch_valid(request->pitch) ||
        !speech_speed_valid(request->speed)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = speech_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    TaskHandle_t worker = NULL;
    speech_lock();
    if (!speech.running || speech.worker == NULL) {
        speech_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (request->drop_if_busy &&
        (speech.current_id != 0U || speech.queue_count > 0U)) {
        speech.dropped++;
        speech_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (speech.queue_count >= SOLAR_OS_SPEECH_QUEUE_CAPACITY) {
        speech.dropped++;
        speech_unlock();
        return ESP_ERR_NO_MEM;
    }

    speech_queue_entry_t *entry = &speech.queue[speech.queue_count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = speech.next_id++;
    if (speech.next_id == 0U) {
        speech.next_id = 1U;
    }
    entry->volume = request->volume;
    entry->pitch = request->pitch != 0U ?
        request->pitch : SOLAR_OS_SPEECH_PITCH_DEFAULT;
    entry->speed = request->speed != 0U ?
        request->speed : SOLAR_OS_SPEECH_SPEED_DEFAULT;
    entry->drop_if_busy = request->drop_if_busy;
    memcpy(entry->text, request->text, request->text_len);
    entry->text[request->text_len] = '\0';
    if (request_id != NULL) {
        *request_id = entry->id;
    }
    worker = speech.worker;
    speech_unlock();
    xTaskNotifyGive(worker);
    return ESP_OK;
}

esp_err_t solar_os_speech_cancel(uint32_t request_id)
{
    if (request_id == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (speech_ensure_mutex() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t worker = NULL;
    speech_lock();
    if (speech.current_id == request_id) {
        speech.cancel_current = true;
        worker = speech.worker;
        speech_unlock();
        if (worker != NULL) {
            xTaskNotifyGive(worker);
        }
        return ESP_OK;
    }
    for (size_t i = 0U; i < speech.queue_count; i++) {
        if (speech.queue[i].id != request_id) {
            continue;
        }
        const size_t progress_total = strlen(speech.queue[i].text);
        for (size_t next = i + 1U; next < speech.queue_count; next++) {
            speech.queue[next - 1U] = speech.queue[next];
        }
        speech.queue_count--;
        speech_store_result_locked(request_id,
                                   SOLAR_OS_SPEECH_REQUEST_CANCELLED,
                                   ESP_ERR_TIMEOUT,
                                   0U,
                                   progress_total);
        speech_unlock();
        return ESP_OK;
    }
    speech_unlock();
    return ESP_ERR_NOT_FOUND;
}

bool solar_os_speech_request_status(uint32_t request_id,
                                    solar_os_speech_request_status_t *status)
{
    if (request_id == 0U || status == NULL || speech_ensure_mutex() != ESP_OK) {
        return false;
    }

    bool found = false;
    speech_lock();
    if (speech.current_id == request_id) {
        *status = (solar_os_speech_request_status_t){
            .id = request_id,
            .state = speech.current_state,
            .error = ESP_OK,
            .progress_done = speech.current_progress_done,
            .progress_total = speech.current_progress_total,
        };
        found = true;
    }
    for (size_t i = 0U; !found && i < speech.queue_count; i++) {
        if (speech.queue[i].id == request_id) {
            *status = (solar_os_speech_request_status_t){
                .id = request_id,
                .state = SOLAR_OS_SPEECH_REQUEST_QUEUED,
                .error = ESP_OK,
                .progress_done = 0U,
                .progress_total = strlen(speech.queue[i].text),
            };
            found = true;
        }
    }
    for (size_t offset = 0U; !found && offset < speech.result_count; offset++) {
        const size_t index =
            (speech.result_next + SOLAR_OS_SPEECH_RESULT_CAPACITY - 1U - offset) %
            SOLAR_OS_SPEECH_RESULT_CAPACITY;
        if (speech.results[index].id == request_id) {
            *status = speech.results[index];
            found = true;
        }
    }
    speech_unlock();
    return found;
}

void solar_os_speech_queue_get_status(solar_os_speech_queue_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    if (speech_ensure_mutex() != ESP_OK) {
        return;
    }
    speech_lock();
    *status = (solar_os_speech_queue_status_t){
        .running = speech.running,
        .queued = speech.queue_count,
        .current_id = speech.current_id,
        .current_state = speech.current_state,
        .completed = speech.completed,
        .cancelled = speech.cancelled,
        .dropped = speech.dropped,
        .failed = speech.failed,
    };
    speech_unlock();
}

const char *solar_os_speech_request_state_name(
    solar_os_speech_request_state_t state)
{
    switch (state) {
    case SOLAR_OS_SPEECH_REQUEST_QUEUED:
        return "queued";
    case SOLAR_OS_SPEECH_REQUEST_WAITING_AUDIO:
        return "waiting_audio";
    case SOLAR_OS_SPEECH_REQUEST_SPEAKING:
        return "speaking";
    case SOLAR_OS_SPEECH_REQUEST_COMPLETE:
        return "complete";
    case SOLAR_OS_SPEECH_REQUEST_CANCELLED:
        return "cancelled";
    case SOLAR_OS_SPEECH_REQUEST_DROPPED:
        return "dropped";
    case SOLAR_OS_SPEECH_REQUEST_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

esp_err_t solar_os_speech_worker_start(TaskHandle_t task)
{
    if (task == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = speech_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    speech_lock();
    if (speech.running) {
        speech_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    speech.worker = task;
    speech.running = true;
    speech.queue_count = 0U;
    speech.result_count = 0U;
    speech.result_next = 0U;
    speech.current_id = 0U;
    speech.current_state = SOLAR_OS_SPEECH_REQUEST_QUEUED;
    speech.current_progress_done = 0U;
    speech.current_progress_total = 0U;
    speech.cancel_current = false;
    speech.completed = 0U;
    speech.cancelled = 0U;
    speech.dropped = 0U;
    speech.failed = 0U;
    speech_unlock();
    return ESP_OK;
}

void solar_os_speech_worker_stop(void)
{
    if (speech_ensure_mutex() != ESP_OK) {
        return;
    }
    TaskHandle_t worker = NULL;
    speech_lock();
    speech.running = false;
    speech.cancel_current = speech.current_id != 0U;
    for (size_t i = 0U; i < speech.queue_count; i++) {
        speech_store_result_locked(speech.queue[i].id,
                                   SOLAR_OS_SPEECH_REQUEST_CANCELLED,
                                   ESP_ERR_TIMEOUT,
                                   0U,
                                   strlen(speech.queue[i].text));
    }
    speech.queue_count = 0U;
    worker = speech.worker;
    speech_unlock();
    if (worker != NULL) {
        xTaskNotifyGive(worker);
    }
}

esp_err_t solar_os_speech_worker_take(solar_os_speech_work_t *work,
                                      uint32_t timeout_ms)
{
    if (work == NULL || speech_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    const TickType_t wait = timeout_ms == UINT32_MAX ?
        portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        speech_lock();
        if (speech.queue_count > 0U) {
            const speech_queue_entry_t entry = speech.queue[0];
            for (size_t i = 1U; i < speech.queue_count; i++) {
                speech.queue[i - 1U] = speech.queue[i];
            }
            speech.queue_count--;
            speech.current_id = entry.id;
            speech.current_state = SOLAR_OS_SPEECH_REQUEST_WAITING_AUDIO;
            speech.current_progress_done = 0U;
            speech.current_progress_total = strlen(entry.text);
            speech.cancel_current = false;
            *work = (solar_os_speech_work_t){
                .id = entry.id,
                .volume = entry.volume,
                .pitch = entry.pitch,
                .speed = entry.speed,
                .drop_if_busy = entry.drop_if_busy,
            };
            strlcpy(work->text, entry.text, sizeof(work->text));
            speech_unlock();
            return ESP_OK;
        }
        const bool running = speech.running;
        const TaskHandle_t worker = speech.worker;
        speech_unlock();
        if (!running) {
            return ESP_ERR_INVALID_STATE;
        }
        if (worker != xTaskGetCurrentTaskHandle()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ulTaskNotifyTake(pdTRUE, wait) == 0U) {
            return ESP_ERR_TIMEOUT;
        }
    }
}

esp_err_t solar_os_speech_worker_set_state(uint32_t request_id,
                                           solar_os_speech_request_state_t state)
{
    if (request_id == 0U || speech_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    speech_lock();
    if (speech.current_id != request_id) {
        speech_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    speech.current_state = state;
    speech_unlock();
    return ESP_OK;
}

esp_err_t solar_os_speech_worker_set_progress(uint32_t request_id,
                                              size_t bytes_done,
                                              size_t bytes_total)
{
    if (request_id == 0U || bytes_done > bytes_total ||
        speech_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    speech_lock();
    if (speech.current_id != request_id) {
        speech_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    speech.current_progress_done = bytes_done;
    speech.current_progress_total = bytes_total;
    speech_unlock();
    return ESP_OK;
}

esp_err_t solar_os_speech_worker_finish(uint32_t request_id,
                                        solar_os_speech_request_state_t state,
                                        esp_err_t error)
{
    if (request_id == 0U || speech_ensure_mutex() != ESP_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    speech_lock();
    if (speech.current_id != request_id) {
        speech_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    if (speech.cancel_current) {
        state = SOLAR_OS_SPEECH_REQUEST_CANCELLED;
        error = ESP_ERR_TIMEOUT;
    }
    const size_t progress_total = speech.current_progress_total;
    const size_t progress_done = state == SOLAR_OS_SPEECH_REQUEST_COMPLETE ?
        progress_total : speech.current_progress_done;
    speech_store_result_locked(request_id,
                               state,
                               error,
                               progress_done,
                               progress_total);
    speech.current_id = 0U;
    speech.current_state = SOLAR_OS_SPEECH_REQUEST_QUEUED;
    speech.current_progress_done = 0U;
    speech.current_progress_total = 0U;
    speech.cancel_current = false;
    speech_unlock();
    return ESP_OK;
}

const volatile bool *solar_os_speech_worker_cancel_flag(uint32_t request_id)
{
    if (request_id == 0U || speech.current_id != request_id) {
        return NULL;
    }
    return &speech.cancel_current;
}
