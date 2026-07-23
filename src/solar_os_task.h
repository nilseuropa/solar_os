#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SOLAR_OS_TASK_STOP_WAIT_MS 2000U
#define SOLAR_OS_TASK_STOP_POLL_MS 20U

BaseType_t solar_os_task_create_pinned(TaskFunction_t task,
                                        const char *name,
                                        uint32_t stack_depth,
                                        void *parameters,
                                        UBaseType_t priority,
                                        TaskHandle_t *handle,
                                        BaseType_t core_id);

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
                                                BaseType_t core_id);

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
                                                BaseType_t core_id);

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
