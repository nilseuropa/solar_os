#include "solar_os_remote_input.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define REMOTE_INPUT_QUEUE_SIZE 64U

static char input_queue[REMOTE_INPUT_QUEUE_SIZE];
static size_t input_head;
static size_t input_count;
static SemaphoreHandle_t input_mutex;
static StaticSemaphore_t input_mutex_storage;

static bool input_lock(void)
{
    if (input_mutex == NULL) {
        /* First caller creates the mutex; both potential first callers
         * (job start, main loop drain) run before any concurrent use
         * can exist, so plain lazy init is fine here. */
        input_mutex = xSemaphoreCreateMutexStatic(&input_mutex_storage);
    }
    return xSemaphoreTake(input_mutex, pdMS_TO_TICKS(100)) == pdTRUE;
}

static void input_unlock(void)
{
    xSemaphoreGive(input_mutex);
}

esp_err_t solar_os_remote_input_push(const char *chars, size_t len)
{
    if (chars == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return ESP_OK;
    }
    if (!input_lock()) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    for (size_t i = 0; i < len; i++) {
        if (input_count >= REMOTE_INPUT_QUEUE_SIZE) {
            ret = ESP_ERR_NO_MEM;
            break;
        }
        input_queue[(input_head + input_count) % REMOTE_INPUT_QUEUE_SIZE] = chars[i];
        input_count++;
    }

    input_unlock();
    return ret;
}

size_t solar_os_remote_input_read_chars(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return 0;
    }
    if (input_mutex == NULL || input_count == 0) {
        /* Racy pre-check is safe: a missed just-pushed char is picked
         * up on the next tick. */
        return 0;
    }
    if (!input_lock()) {
        return 0;
    }

    size_t copied = 0;
    while (copied < buffer_len && input_count > 0) {
        buffer[copied++] = input_queue[input_head];
        input_head = (input_head + 1) % REMOTE_INPUT_QUEUE_SIZE;
        input_count--;
    }

    input_unlock();
    return copied;
}
