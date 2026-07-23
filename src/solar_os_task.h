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
 * Keep a task stack in internal SRAM even when PSRAM-backed task stacks are
 * available. Use this only when the worker can run while the flash/cache is
 * unavailable, services DMA/ISR-adjacent work, or has another documented
 * internal-memory requirement.
 */
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id);

/*
 * Pair these with the matching creation function. The default lifecycle uses
 * PSRAM for its stack on PSRAM-enabled builds and normal internal allocation
 * on boards without PSRAM.
 */
void solar_os_task_delete(TaskHandle_t task);
void solar_os_task_delete_internal(TaskHandle_t task);

bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms);
