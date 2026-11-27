#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "solar_os_ota.h"

#define OTA_WORKER_STACK 16384U
#define OTA_WORKER_WAIT_MS 100U
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY -1
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define pdMS_TO_TICKS(ms) (ms)
typedef unsigned TickType_t;

static const char *TAG = "ota";
static atomic_flag ota_operation = ATOMIC_FLAG_INIT;
static pthread_t worker_thread;
static bool fail_launch;
static bool updated;
static unsigned launches;
static esp_err_t simulated_result;

static void test_log(const char *tag, const char *format, ...)
{
    assert(tag == TAG && format);
}
#define SOLAR_OS_LOGI(...) test_log(__VA_ARGS__)
#define SOLAR_OS_LOGW(...) test_log(__VA_ARGS__)

static unsigned uxTaskGetStackHighWaterMark(void *task)
{
    assert(task == NULL);
    return 2048;
}

static void solar_os_task_delete_internal(void *task)
{
    assert(task == NULL);
}

static void vTaskDelay(TickType_t ticks)
{
    assert(ticks > 0);
    nanosleep(&(struct timespec){.tv_nsec = 1000000}, NULL);
}

typedef struct {
    void (*run)(void *);
    void *argument;
} test_task_t;
static test_task_t task;

static void *run_worker(void *unused)
{
    assert(unused == NULL);
    task.run(task.argument);
    return NULL;
}

static int solar_os_task_create_pinned_internal(void (*run)(void *), const char *name,
    unsigned stack_depth, void *parameters, unsigned priority, void *handle, int core, int role)
{
    assert(run && name && stack_depth == OTA_WORKER_STACK && parameters);
    assert(priority == tskIDLE_PRIORITY + 2 && handle == NULL);
    assert(core == tskNO_AFFINITY && role == SOLAR_OS_TASK_ROLE_FOREGROUND);
    ++launches;
    if (fail_launch) return 0;
    task = (test_task_t){.run = run, .argument = parameters};
    assert(!pthread_create(&worker_thread, NULL, run_worker, NULL));
    return pdPASS;
}

static esp_err_t ota_check(solar_os_ota_check_result_t *result)
{
    assert(pthread_equal(pthread_self(), worker_thread));
    result->index_signature_verified = true;
    result->status_code = 200;
    return simulated_result;
}

static esp_err_t ota_upgrade(solar_os_ota_progress_cb_t progress, void *user)
{
    assert(pthread_equal(pthread_self(), worker_thread));
    const solar_os_ota_progress_t event = {.stage = SOLAR_OS_OTA_PROGRESS_WRITING};
    if (progress) progress(&event, user);
    return simulated_result;
}

#include "ota_worker.inc"

static void progress(const solar_os_ota_progress_t *event, void *user)
{
    assert(!updated && user == &updated && event->stage == SOLAR_OS_OTA_PROGRESS_WRITING);
    solar_os_ota_check_result_t result = {0};
    assert(solar_os_ota_check(&result) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ota_upgrade(NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(!result.status_code);
    updated = true;
}

int main(void)
{
    solar_os_ota_check_result_t result = {0};
    assert(solar_os_ota_check(NULL) == ESP_ERR_INVALID_ARG && !launches);
    fail_launch = true;
    assert(solar_os_ota_check(&result) == ESP_ERR_NO_MEM && launches == 1);
    assert(!result.index_signature_verified);
    fail_launch = false;
    simulated_result = ESP_ERR_TIMEOUT;
    assert(solar_os_ota_upgrade(progress, &updated) == ESP_ERR_TIMEOUT);
    assert(!pthread_join(worker_thread, NULL) && updated && launches == 2);
    simulated_result = ESP_OK;
    assert(solar_os_ota_check(&result) == ESP_OK);
    assert(!pthread_join(worker_thread, NULL) && launches == 3);
    assert(result.index_signature_verified && result.status_code == 200);
    return 0;
}
