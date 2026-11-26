#include "solar_os_radio_scan.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define SCAN_WORKER_STACK 4096U
#define SCAN_WORKER_STOP_MS 3000U

struct solar_os_radio_scan_worker {
    solar_os_radio_scan_t scan;
    StaticSemaphore_t lock_storage;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    atomic_bool stopping;
    atomic_bool finished;
};

static void scan_worker_task(void *argument)
{
    solar_os_radio_scan_worker_t *worker = argument;
    while (!atomic_load_explicit(&worker->stopping, memory_order_acquire)) {
        xSemaphoreTake(worker->lock, portMAX_DELAY);
        (void)solar_os_radio_scan_poll(&worker->scan);
        bool running = worker->scan.running;
        int64_t wait_us = worker->scan.next_us - esp_timer_get_time();
        xSemaphoreGive(worker->lock);
        if (!running) break;
        if (wait_us > 0) {
            TickType_t wait_ticks = pdMS_TO_TICKS((wait_us + 999) / 1000);
            if (wait_ticks == 0) wait_ticks = 1;
            ulTaskNotifyTake(pdTRUE, wait_ticks);
        }
        /* A sampled bin can immediately retune the next frequency. Only the
         * driver's settling interval sleeps; GUI cadence never paces sampling. */
    }
    xSemaphoreTake(worker->lock, portMAX_DELAY);
    esp_err_t sample_error = worker->scan.error;
    esp_err_t restore_error = solar_os_radio_scan_stop(&worker->scan);
    if (sample_error != ESP_OK) worker->scan.error = sample_error;
    else worker->scan.error = restore_error;
    xSemaphoreGive(worker->lock);
    /* The owner deletes the parked task after observing finished. The task
     * handle therefore remains valid even if sampling fails before Close. No
     * worker memory may be touched after publishing this release barrier. */
    atomic_store_explicit(&worker->finished, true, memory_order_release);
    vTaskSuspend(NULL);
}

esp_err_t solar_os_radio_scan_worker_open(solar_os_radio_scan_worker_t **out,
    const char *name, const solar_os_radio_scan_config_t *config)
{
    if (out == NULL || *out != NULL || name == NULL || config == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_radio_scan_worker_t *worker = solar_os_memory_calloc(1, sizeof(*worker),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "radio.scan.worker");
    if (worker == NULL) return ESP_ERR_NO_MEM;
    worker->lock = xSemaphoreCreateMutexStatic(&worker->lock_storage);
    if (!worker->lock) {
        solar_os_memory_free(worker);
        return ESP_ERR_NO_MEM;
    }
    atomic_init(&worker->stopping, false);
    atomic_init(&worker->finished, false);
    solar_os_radio_scan_init(&worker->scan);
    esp_err_t err = solar_os_radio_scan_start(&worker->scan, name, config);
    if (err == ESP_OK && solar_os_task_create_pinned_internal(scan_worker_task, "radio-scan",
            SCAN_WORKER_STACK, worker, tskIDLE_PRIORITY + 1U, &worker->task,
            tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        (void)solar_os_radio_scan_stop(&worker->scan);
    }
    if (err != ESP_OK && worker->scan.handle.index >= 0) {
        worker->scan.error = err;
        *out = worker;   /* Failed start still owns a retiring radio. */
        return err;
    }
    if (err != ESP_OK) {
        vSemaphoreDelete(worker->lock);
        solar_os_memory_free(worker);
        return err;
    }
    *out = worker;
    return ESP_OK;
}

esp_err_t solar_os_radio_scan_worker_snapshot(solar_os_radio_scan_worker_t *worker,
    solar_os_radio_scan_snapshot_fn copy, void *user)
{
    if (!worker || !copy) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(worker->lock, pdMS_TO_TICKS(10)) != pdTRUE) return ESP_ERR_TIMEOUT;
    copy(&worker->scan, user);
    worker->scan.recent_peak = SOLAR_OS_RADIO_SCAN_EMPTY;
    worker->scan.recent_peak_frequency_hz = 0;
    xSemaphoreGive(worker->lock);
    return ESP_OK;
}

static void snapshot_copy(const solar_os_radio_scan_t *scan, void *user)
{
    memcpy(user, scan, sizeof(*scan));
}

esp_err_t solar_os_radio_scan_worker_copy(solar_os_radio_scan_worker_t *worker,
    solar_os_radio_scan_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    return solar_os_radio_scan_worker_snapshot(worker, snapshot_copy, snapshot);
}

esp_err_t solar_os_radio_scan_worker_clear(solar_os_radio_scan_worker_t *worker)
{
    if (worker == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(worker->lock, portMAX_DELAY);
    solar_os_radio_scan_clear(&worker->scan);
    xSemaphoreGive(worker->lock);
    /* The task can already be parked after an error, but remains valid until
     * Close deletes it. Notifications cannot dereference a stale task handle. */
    if (worker->task) xTaskNotifyGive(worker->task);
    return ESP_OK;
}

bool solar_os_radio_scan_worker_done(const solar_os_radio_scan_worker_t *worker)
{
    return worker == NULL || worker->task == NULL ||
        (atomic_load_explicit(&worker->finished, memory_order_acquire) &&
         eTaskGetState(worker->task) == eSuspended);
}

esp_err_t solar_os_radio_scan_worker_close_snapshot(solar_os_radio_scan_worker_t **out,
    solar_os_radio_scan_snapshot_fn copy, void *user)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_radio_scan_worker_t *worker = *out;
    if (worker == NULL) return ESP_OK;
    if (worker->task) {
        atomic_store_explicit(&worker->stopping, true, memory_order_release);
        xTaskNotifyGive(worker->task);
        int64_t deadline = esp_timer_get_time() + SCAN_WORKER_STOP_MS * 1000LL;
        while (!solar_os_radio_scan_worker_done(worker)) {
            if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
            vTaskDelay(1);
        }
        solar_os_task_delete_internal(worker->task);
        worker->task = NULL;
    }
    const esp_err_t result = worker->scan.error;
    if (worker->scan.handle.index >= 0) {
        const esp_err_t error = solar_os_radio_release(&worker->scan.handle);
        if (error != ESP_OK) return error;
    }
    if (copy) copy(&worker->scan, user);
    vSemaphoreDelete(worker->lock);
    solar_os_memory_free(worker);
    *out = NULL;
    return result;
}

esp_err_t solar_os_radio_scan_worker_close(solar_os_radio_scan_worker_t **out,
    solar_os_radio_scan_t *snapshot)
{
    return solar_os_radio_scan_worker_close_snapshot(out, snapshot ? snapshot_copy : NULL, snapshot);
}
