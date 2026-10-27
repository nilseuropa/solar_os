#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../../src/services/solar_os_radio_scan.c"
#include "../../src/services/solar_os_radio_scan_worker.c"

struct test_scan_task {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned notifications;
    TaskFunction_t function;
    void *argument;
    atomic_bool parked;
};
static _Thread_local TaskHandle_t current;
static bool fail_create;
static unsigned allocations, configurations, tunes;
static bool claimed;
static atomic_bool fail_read, block_read, reading;
static solar_os_radio_status_t device;

int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
void vTaskDelay(TickType_t ms)
{
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&t, NULL);
}
static void *entry(void *argument)
{ current = argument; current->function(current->argument); return NULL; }
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t fn, const char *name,
    uint32_t stack, void *arg, UBaseType_t priority, TaskHandle_t *out,
    BaseType_t core, solar_os_task_role_t role)
{
    (void)name; (void)priority; (void)core; (void)role; assert(stack >= 4096);
    if (fail_create) return pdFALSE;
    TaskHandle_t task = calloc(1, sizeof(*task)); assert(task);
    pthread_mutex_init(&task->lock, NULL); pthread_cond_init(&task->wake, NULL);
    task->function = fn; task->argument = arg; *out = task;
    atomic_init(&task->parked, false);
    assert(pthread_create(&task->thread, NULL, entry, task) == 0); return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t task)
{
    pthread_mutex_lock(&task->lock); task->notifications++;
    pthread_cond_signal(&task->wake); pthread_mutex_unlock(&task->lock); return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ms)
{
    TaskHandle_t task = current;
    pthread_mutex_lock(&task->lock);
    struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec += ms / 1000; t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    while (!task->notifications) {
        if (pthread_cond_timedwait(&task->wake, &task->lock, &t) == ETIMEDOUT) break;
    }
    unsigned count = task->notifications;
    if (clear) task->notifications = 0;
    pthread_mutex_unlock(&task->lock); return count;
}
void vTaskSuspend(TaskHandle_t task)
{ assert(task == NULL); atomic_store(&current->parked, true); pthread_exit(NULL); }
eTaskState eTaskGetState(TaskHandle_t task)
{ return atomic_load(&task->parked) ? eSuspended : eRunning; }
void solar_os_task_delete_internal(TaskHandle_t task)
{
    assert(pthread_join(task->thread, NULL) == 0);
    pthread_cond_destroy(&task->wake); pthread_mutex_destroy(&task->lock); free(task);
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; allocations++; return calloc(n, size); }
void solar_os_memory_free(void *p) { if (p) { allocations--; free(p); } }
size_t strlcpy(char *out, const char *in, size_t size)
{
    size_t n = strlen(in);
    if (size) { size_t copy = n < size - 1 ? n : size - 1; memcpy(out, in, copy); out[copy] = 0; }
    return n;
}
esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *info)
{
    (void)name; memset(info, 0, sizeof(*info));
    info->features = SOLAR_OS_RADIO_FEATURE_LIVE_RSSI | SOLAR_OS_RADIO_FEATURE_FAST_TUNE;
    info->claimed = claimed; if (claimed) strlcpy(info->owner, "rfscan", sizeof(info->owner));
    return ESP_OK;
}
esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *out)
{ (void)name; *out = device; return ESP_OK; }
esp_err_t solar_os_radio_claim(const char *name, const char *owner, solar_os_radio_handle_t *handle)
{
    (void)name; (void)owner;
    if (claimed) return ESP_ERR_INVALID_STATE;
    claimed = true; handle->index = 0; handle->token = 1; return ESP_OK;
}
esp_err_t solar_os_radio_release(solar_os_radio_handle_t *handle)
{ assert(claimed); claimed = false; handle->index = -1; handle->token = 0; return ESP_OK; }
esp_err_t solar_os_radio_handle_get_status(const solar_os_radio_handle_t *handle, solar_os_radio_status_t *out)
{ (void)handle; *out = device; return ESP_OK; }
esp_err_t solar_os_radio_handle_configure(const solar_os_radio_handle_t *handle, const solar_os_radio_config_t *config)
{ (void)handle; assert(claimed); configurations++; device.config = *config; device.state = SOLAR_OS_RADIO_STATE_STANDBY; return ESP_OK; }
esp_err_t solar_os_radio_handle_set_state(const solar_os_radio_handle_t *handle, solar_os_radio_state_t state)
{ (void)handle; device.state = state; return ESP_OK; }
esp_err_t solar_os_radio_handle_tune_rx(const solar_os_radio_handle_t *handle, uint32_t frequency, uint32_t *settle_ms)
{ (void)handle; assert(claimed); tunes++; device.config.frequency_hz = frequency; device.state = SOLAR_OS_RADIO_STATE_RX; *settle_ms = 2; return ESP_OK; }
esp_err_t solar_os_radio_read_rssi(const char *name, int16_t *dbm)
{
    (void)name; atomic_store(&reading, true);
    while (atomic_load(&block_read)) vTaskDelay(1);
    if (atomic_load(&fail_read)) return ESP_FAIL;
    *dbm = -110 + (device.config.frequency_hz / 125000) % 8; return ESP_OK;
}

int main(void)
{
    solar_os_radio_scan_worker_t *worker = NULL, *other = NULL;
    device.config.frequency_hz = 869618000;
    device.config.rx_bandwidth_hz = 62500;
    device.state = SOLAR_OS_RADIO_STATE_SLEEP;
    solar_os_radio_status_t saved = device;
    solar_os_radio_scan_config_t config = {.mode = SOLAR_OS_RADIO_SCAN_BAND,
        .start_hz = 868000000, .end_hz = 868250000, .step_hz = 125000};
    solar_os_radio_scan_t view, frozen;
    assert(solar_os_radio_scan_worker_open(&worker, "radio0", &config) == ESP_OK);
    assert(solar_os_radio_scan_worker_open(&other, "radio0", &config) == ESP_ERR_INVALID_STATE && other == NULL);
    vTaskDelay(25); assert(solar_os_radio_scan_worker_copy(worker, &view) == ESP_OK);
    assert(view.sweeps >= 2); frozen = view;
    vTaskDelay(60); /* Simulate a slow redraw without touching or polling the worker. */
    assert(solar_os_radio_scan_worker_copy(worker, &view) == ESP_OK && view.sweeps > frozen.sweeps);
    assert(frozen.generation != view.generation && configurations == 1);
    assert(solar_os_radio_scan_worker_close(&worker, &view) == ESP_OK && worker == NULL);
    assert(!claimed && memcmp(&device, &saved, sizeof(saved)) == 0 && allocations == 0);

    /* Fresh peaks survive until copied and are consumed exactly once. A
     * retained latest value during a quiet interval cannot repeat an alert. */
    solar_os_radio_scan_config_t channel = {.mode = SOLAR_OS_RADIO_SCAN_CHANNEL, .interval_ms = 3600000};
    assert(solar_os_radio_scan_worker_open(&worker, "radio0", &channel) == ESP_OK);
    vTaskDelay(40);
    assert(solar_os_radio_scan_worker_copy(worker, &view) == ESP_OK);
    assert(view.recent_peak != SOLAR_OS_RADIO_SCAN_EMPTY && view.recent_peak == view.latest);
    assert(view.recent_peak_frequency_hz == device.config.frequency_hz);
    assert(solar_os_radio_scan_worker_copy(worker, &view) == ESP_OK);
    assert(view.recent_peak == SOLAR_OS_RADIO_SCAN_EMPTY && view.latest != SOLAR_OS_RADIO_SCAN_EMPTY);
    assert(view.recent_peak_frequency_hz == 0);
    assert(solar_os_radio_scan_worker_close(&worker, &view) == ESP_OK);
    assert(!claimed && memcmp(&device, &saved, sizeof(saved)) == 0 && allocations == 0);

    fail_create = true;
    assert(solar_os_radio_scan_worker_open(&worker, "radio0", &config) == ESP_ERR_NO_MEM && worker == NULL);
    assert(!claimed && device.state == saved.state && allocations == 0);
    fail_create = false;

    atomic_store(&fail_read, true);
    assert(solar_os_radio_scan_worker_open(&worker, "radio0", &config) == ESP_OK);
    while (!solar_os_radio_scan_worker_done(worker)) vTaskDelay(1);
    /* A worker that failed before Close must retain a valid task handle. */
    assert(solar_os_radio_scan_worker_close(&worker, &view) == ESP_FAIL);
    assert(worker == NULL && !claimed && allocations == 0);
    atomic_store(&fail_read, false);

    atomic_store(&reading, false); atomic_store(&block_read, true);
    assert(solar_os_radio_scan_worker_open(&worker, "radio0", &config) == ESP_OK);
    while (!atomic_load(&reading)) vTaskDelay(1);
    assert(solar_os_radio_scan_worker_copy(worker, &view) == ESP_ERR_TIMEOUT);
    assert(solar_os_radio_scan_worker_close(&worker, &view) == ESP_ERR_TIMEOUT);
    assert(worker != NULL && claimed && !solar_os_radio_scan_worker_done(worker));
    atomic_store(&block_read, false);
    assert(solar_os_radio_scan_worker_close(&worker, &view) == ESP_OK);
    assert(worker == NULL && !claimed && allocations == 0);
    puts("radio scan worker: independent sampling, snapshots, failure rollback, timed stop and lifetime: ok");
    return 0;
}
