#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_resource_limits.h"

#define SOLAR_OS_TASK_STOP_WAIT_MS 2000U
#define SOLAR_OS_TASK_STOP_POLL_MS 20U
#define SOLAR_OS_TASK_REAP_WAIT_MS 100U
#define SOLAR_OS_TASK_NAME_MAX 24U

typedef enum {
    SOLAR_OS_TASK_ROLE_SYSTEM = 0,
    SOLAR_OS_TASK_ROLE_FOREGROUND,
    SOLAR_OS_TASK_ROLE_BACKGROUND,
    SOLAR_OS_TASK_ROLE_COUNT,
} solar_os_task_role_t;

typedef struct {
    uint32_t requests;
    uint32_t successes;
    uint32_t denied;
    uint32_t failures;
    uint64_t requested_stack_bytes;
} solar_os_task_role_stats_t;

typedef struct {
    solar_os_task_role_stats_t roles[SOLAR_OS_TASK_ROLE_COUNT];
    uint32_t waiting;
    uint32_t wait_successes;
    uint32_t wait_cancellations;
    bool last_failure_valid;
    bool last_failure_denied;
    solar_os_task_role_t last_failure_role;
    uint32_t last_failure_stack_bytes;
    char last_failure_name[SOLAR_OS_TASK_NAME_MAX];
} solar_os_task_status_t;

typedef struct {
    bool launch_locked;
} solar_os_task_managed_admission_t;

BaseType_t solar_os_task_create_pinned(TaskFunction_t task,
                                        const char *name,
                                        uint32_t stack_depth,
                                        void *parameters,
                                        UBaseType_t priority,
                                        TaskHandle_t *handle,
                                        BaseType_t core_id,
                                        solar_os_task_role_t role);

/*
 * Use a PSRAM stack only for a worker audited never to initiate flash access or
 * otherwise run while the external-memory cache is disabled. ESP-IDF asserts
 * if a task with an external stack enters a cache-disabled flash operation.
 * Pair this function with solar_os_task_delete_external().
 */
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role);

/*
 * Explicitly document a worker whose stack must remain in internal SRAM. The
 * default helper is also internal; this name is retained for cache-disabled,
 * DMA, ISR-adjacent, or timing-sensitive workers where placement is part of
 * the worker's contract.
 */
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role);

/*
 * Pair deletion with the matching creation function. Default and explicitly
 * internal workers use solar_os_task_delete(); the explicit internal alias is
 * available where the placement contract should remain visible at exit.
 */
void solar_os_task_delete(TaskHandle_t task);
void solar_os_task_delete_external(TaskHandle_t task);
void solar_os_task_delete_internal(TaskHandle_t task);

bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms);

void solar_os_task_get_status(solar_os_task_status_t *status);
const char *solar_os_task_role_name(solar_os_task_role_t role);
bool solar_os_task_can_create(uint32_t stack_depth,
                              solar_os_task_role_t role,
                              bool external_stack);

/* Preflight a launch. Pair managed admission with note_managed_result(). */
bool solar_os_task_admit(const char *name,
                         uint32_t stack_depth,
                         solar_os_task_role_t role,
                         bool external_stack);
bool solar_os_task_admit_managed(const char *name,
                                 uint32_t stack_depth,
                                 solar_os_task_role_t role,
                                 bool external_stack,
                                 solar_os_task_managed_admission_t *admission);
void solar_os_task_note_managed_result(const char *name,
                                       uint32_t stack_depth,
                                       solar_os_task_role_t role,
                                       solar_os_task_managed_admission_t *admission,
                                       bool success);
void solar_os_task_note_wait_queued(void);
void solar_os_task_note_wait_finished(bool launched);
