#include "solar_os_task.h"

#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM && \
    defined(CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM) && \
    CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 1
#else
#define SOLAR_OS_FREERTOS_EXTERNAL_MEMORY 0
#endif

static portMUX_TYPE task_stats_lock = portMUX_INITIALIZER_UNLOCKED;
static solar_os_task_role_stats_t task_stats[SOLAR_OS_TASK_ROLE_COUNT];
static bool task_last_failure_valid;
static bool task_last_failure_denied;
static solar_os_task_role_t task_last_failure_role;
static uint32_t task_last_failure_stack;
static char task_last_failure_name[SOLAR_OS_TASK_NAME_MAX];
static uint32_t task_waiting;
static uint32_t task_wait_successes;
static uint32_t task_wait_cancellations;
static StaticSemaphore_t task_launch_mutex_storage;
static SemaphoreHandle_t task_launch_mutex;
static portMUX_TYPE task_launch_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;

static bool task_role_valid(solar_os_task_role_t role)
{
    return role >= SOLAR_OS_TASK_ROLE_SYSTEM && role < SOLAR_OS_TASK_ROLE_COUNT;
}

const char *solar_os_task_role_name(solar_os_task_role_t role)
{
    switch (role) {
    case SOLAR_OS_TASK_ROLE_SYSTEM:
        return "system";
    case SOLAR_OS_TASK_ROLE_FOREGROUND:
        return "foreground";
    case SOLAR_OS_TASK_ROLE_BACKGROUND:
        return "background";
    case SOLAR_OS_TASK_ROLE_COUNT:
    default:
        return "invalid";
    }
}

static void task_record_request(solar_os_task_role_t role, uint32_t stack_depth)
{
    portENTER_CRITICAL(&task_stats_lock);
    task_stats[role].requests++;
    task_stats[role].requested_stack_bytes += stack_depth;
    portEXIT_CRITICAL(&task_stats_lock);
}

static void task_record_success(solar_os_task_role_t role)
{
    portENTER_CRITICAL(&task_stats_lock);
    task_stats[role].successes++;
    portEXIT_CRITICAL(&task_stats_lock);
}

static void task_record_failure(solar_os_task_role_t role,
                                const char *name,
                                uint32_t stack_depth,
                                bool denied)
{
    portENTER_CRITICAL(&task_stats_lock);
    if (denied) {
        task_stats[role].denied++;
    } else {
        task_stats[role].failures++;
    }
    task_last_failure_valid = true;
    task_last_failure_denied = denied;
    task_last_failure_role = role;
    task_last_failure_stack = stack_depth;
    strlcpy(task_last_failure_name,
            name != NULL ? name : "unknown",
            sizeof(task_last_failure_name));
    portEXIT_CRITICAL(&task_stats_lock);
}

static bool task_add_size(size_t left, size_t right, size_t *result)
{
    if (result == NULL || left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static SemaphoreHandle_t task_launch_mutex_get(void)
{
    portENTER_CRITICAL(&task_launch_mutex_init_lock);
    if (task_launch_mutex == NULL) {
        task_launch_mutex = xSemaphoreCreateMutexStatic(&task_launch_mutex_storage);
    }
    SemaphoreHandle_t mutex = task_launch_mutex;
    portEXIT_CRITICAL(&task_launch_mutex_init_lock);
    return mutex;
}

static bool task_launch_lock(void)
{
    SemaphoreHandle_t mutex = task_launch_mutex_get();
    return mutex != NULL && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
}

static void task_launch_unlock(void)
{
    if (task_launch_mutex != NULL) {
        xSemaphoreGive(task_launch_mutex);
    }
}

static bool task_internal_admitted(uint32_t stack_depth, solar_os_task_role_t role)
{
    if (role == SOLAR_OS_TASK_ROLE_SYSTEM) {
        return true;
    }

    size_t task_required = 0;
    if (!task_add_size((size_t)stack_depth,
                       SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES,
                       &task_required)) {
        return false;
    }
    size_t total_required = task_required;
    if (role == SOLAR_OS_TASK_ROLE_BACKGROUND &&
        !task_add_size(total_required,
                       SOLAR_OS_INTERNAL_LAUNCH_RESERVE_BYTES,
                       &total_required)) {
        return false;
    }

    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    return heap_caps_get_free_size(caps) >= total_required &&
           heap_caps_get_largest_free_block(caps) >= task_required;
}

#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
static bool task_external_admitted(uint32_t stack_depth, solar_os_task_role_t role)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    size_t internal_required = SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES;
    if (role == SOLAR_OS_TASK_ROLE_BACKGROUND &&
        !task_add_size(internal_required,
                       SOLAR_OS_INTERNAL_LAUNCH_RESERVE_BYTES,
                       &internal_required)) {
        return false;
    }
    return heap_caps_get_free_size(internal_caps) >= internal_required &&
           heap_caps_get_largest_free_block(internal_caps) >=
               SOLAR_OS_TASK_INTERNAL_OVERHEAD_BYTES &&
           heap_caps_get_free_size(external_caps) >= stack_depth &&
           heap_caps_get_largest_free_block(external_caps) >= stack_depth;
}
#endif

bool solar_os_task_can_create(uint32_t stack_depth,
                              solar_os_task_role_t role,
                              bool external_stack)
{
    if (stack_depth == 0 || !task_role_valid(role)) {
        return false;
    }
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    return external_stack ? task_external_admitted(stack_depth, role) :
                            task_internal_admitted(stack_depth, role);
#else
    (void)external_stack;
    return task_internal_admitted(stack_depth, role);
#endif
}

static void task_log_create_failure(const char *name,
                                    uint32_t stack_depth,
                                    solar_os_task_role_t role,
                                    bool denied)
{
    solar_os_memory_status_t memory;
    solar_os_memory_get_status(&memory);
    SOLAR_OS_LOGW("task",
                  "%s name=%s role=%s stack=%u internal_free=%u internal_max=%u external_free=%u",
                  denied ? "admission denied" : "create failed",
                  name != NULL ? name : "unknown",
                  solar_os_task_role_name(role),
                  (unsigned)stack_depth,
                  (unsigned)memory.internal.free,
                  (unsigned)memory.internal.largest_free,
                  (unsigned)memory.external.free);
}

bool solar_os_task_admit(const char *name,
                         uint32_t stack_depth,
                         solar_os_task_role_t role,
                         bool external_stack)
{
    if (stack_depth == 0 || !task_role_valid(role)) {
        return false;
    }
    if (!task_launch_lock()) {
        return false;
    }
    if (solar_os_task_can_create(stack_depth, role, external_stack)) {
        task_launch_unlock();
        return true;
    }

    task_record_request(role, stack_depth);
    task_record_failure(role, name, stack_depth, true);
    task_log_create_failure(name, stack_depth, role, true);
    task_launch_unlock();
    return false;
}

bool solar_os_task_admit_managed(const char *name,
                                 uint32_t stack_depth,
                                 solar_os_task_role_t role,
                                 bool external_stack,
                                 solar_os_task_managed_admission_t *admission)
{
    if (admission == NULL) {
        return false;
    }
    admission->launch_locked = false;
    if (stack_depth == 0 || !task_role_valid(role) || !task_launch_lock()) {
        return false;
    }
    admission->launch_locked = true;
    task_record_request(role, stack_depth);
    if (!solar_os_task_can_create(stack_depth, role, external_stack)) {
        task_record_failure(role, name, stack_depth, true);
        task_log_create_failure(name, stack_depth, role, true);
        task_launch_unlock();
        admission->launch_locked = false;
        return false;
    }
    return true;
}

void solar_os_task_note_managed_result(const char *name,
                                       uint32_t stack_depth,
                                       solar_os_task_role_t role,
                                       solar_os_task_managed_admission_t *admission,
                                       bool success)
{
    if (admission != NULL) {
        if (admission->launch_locked) {
            task_launch_unlock();
            admission->launch_locked = false;
        }
    }
    if (!task_role_valid(role)) {
        return;
    }
    if (success) {
        task_record_success(role);
        return;
    }
    task_record_failure(role, name, stack_depth, false);
    task_log_create_failure(name, stack_depth, role, false);
}

void solar_os_task_note_wait_queued(void)
{
    portENTER_CRITICAL(&task_stats_lock);
    task_waiting++;
    portEXIT_CRITICAL(&task_stats_lock);
}

void solar_os_task_note_wait_finished(bool launched)
{
    portENTER_CRITICAL(&task_stats_lock);
    if (task_waiting > 0) {
        task_waiting--;
    }
    if (launched) {
        task_wait_successes++;
    } else {
        task_wait_cancellations++;
    }
    portEXIT_CRITICAL(&task_stats_lock);
}

BaseType_t solar_os_task_create_pinned(TaskFunction_t task,
                                        const char *name,
                                        uint32_t stack_depth,
                                        void *parameters,
                                        UBaseType_t priority,
                                        TaskHandle_t *handle,
                                        BaseType_t core_id,
                                        solar_os_task_role_t role)
{
    if (task == NULL || stack_depth == 0 || !task_role_valid(role)) {
        return pdFAIL;
    }
    if (!task_launch_lock()) {
        return pdFAIL;
    }
    task_record_request(role, stack_depth);
    if (!solar_os_task_can_create(stack_depth, role, false)) {
        task_record_failure(role, name, stack_depth, true);
        task_log_create_failure(name, stack_depth, role, true);
        task_launch_unlock();
        return pdFAIL;
    }

    const BaseType_t result = xTaskCreatePinnedToCore(task,
                                                      name,
                                                      stack_depth,
                                                      parameters,
                                                      priority,
                                                      handle,
                                                      core_id);
    task_launch_unlock();
    if (result != pdPASS) {
        task_record_failure(role, name, stack_depth, false);
        task_log_create_failure(name, stack_depth, role, false);
    } else {
        task_record_success(role);
    }
    return result;
}

BaseType_t solar_os_task_create_pinned_external(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role)
{
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    if (task == NULL || stack_depth == 0 || !task_role_valid(role)) {
        return pdFAIL;
    }
    if (!task_launch_lock()) {
        return pdFAIL;
    }
    task_record_request(role, stack_depth);
    if (!solar_os_task_can_create(stack_depth, role, true)) {
        task_record_failure(role, name, stack_depth, true);
        task_log_create_failure(name, stack_depth, role, true);
        task_launch_unlock();
        return pdFAIL;
    }

    const BaseType_t result = xTaskCreatePinnedToCoreWithCaps(
        task,
        name,
        stack_depth,
        parameters,
        priority,
        handle,
        core_id,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    task_launch_unlock();
    if (result != pdPASS) {
        task_record_failure(role, name, stack_depth, false);
        task_log_create_failure(name, stack_depth, role, false);
    } else {
        task_record_success(role);
    }
    return result;
#else
    return solar_os_task_create_pinned(task,
                                       name,
                                       stack_depth,
                                       parameters,
                                       priority,
                                       handle,
                                       core_id,
                                       role);
#endif
}

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role)
{
    return solar_os_task_create_pinned(task,
                                       name,
                                       stack_depth,
                                       parameters,
                                       priority,
                                       handle,
                                       core_id,
                                       role);
}

void solar_os_task_delete(TaskHandle_t task)
{
    vTaskDelete(task);
}

void solar_os_task_delete_external(TaskHandle_t task)
{
#if SOLAR_OS_FREERTOS_EXTERNAL_MEMORY
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
#endif
}

void solar_os_task_delete_internal(TaskHandle_t task)
{
    vTaskDelete(task);
}

bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms)
{
    if (task == NULL) {
        return true;
    }

    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && timeout_ticks == 0) {
        timeout_ticks = 1;
    }

    TickType_t poll_ticks = pdMS_TO_TICKS(SOLAR_OS_TASK_STOP_POLL_MS);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }

    const TickType_t start = xTaskGetTickCount();
    while (task_done == NULL || !*task_done) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (timeout_ticks == 0 || elapsed >= timeout_ticks) {
            return false;
        }

        const TickType_t remaining = timeout_ticks - elapsed;
        vTaskDelay(remaining < poll_ticks ? remaining : poll_ticks);
    }

    /* Workers set task_done immediately before self-deleting. Give the worker
     * and the idle task a scheduling window to reclaim its dynamic stack so an
     * immediate restart does not see the old task's allocation. One scheduler
     * tick is insufficient on a busy dual-core target. */
    TickType_t reap_ticks = pdMS_TO_TICKS(SOLAR_OS_TASK_REAP_WAIT_MS);
    if (reap_ticks == 0) {
        reap_ticks = 1;
    }
    vTaskDelay(reap_ticks);
    return true;
}

void solar_os_task_get_status(solar_os_task_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    portENTER_CRITICAL(&task_stats_lock);
    memcpy(status->roles, task_stats, sizeof(task_stats));
    status->waiting = task_waiting;
    status->wait_successes = task_wait_successes;
    status->wait_cancellations = task_wait_cancellations;
    status->last_failure_valid = task_last_failure_valid;
    status->last_failure_denied = task_last_failure_denied;
    status->last_failure_role = task_last_failure_role;
    status->last_failure_stack_bytes = task_last_failure_stack;
    memcpy(status->last_failure_name,
           task_last_failure_name,
           sizeof(task_last_failure_name));
    portEXIT_CRITICAL(&task_stats_lock);
}
