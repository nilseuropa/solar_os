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

bool solar_os_task_wait_done(TaskHandle_t task,
                             volatile bool *task_done,
                             uint32_t timeout_ms);
