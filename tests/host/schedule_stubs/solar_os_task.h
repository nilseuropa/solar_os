#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int BaseType_t;
typedef unsigned UBaseType_t;

#define pdPASS 1
#define tskNO_AFFINITY 0

typedef enum {
    SOLAR_OS_TASK_ROLE_BACKGROUND,
} solar_os_task_role_t;

BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t task,
                                                const char *name,
                                                uint32_t stack_depth,
                                                void *parameters,
                                                UBaseType_t priority,
                                                TaskHandle_t *handle,
                                                BaseType_t core_id,
                                                solar_os_task_role_t role);
void solar_os_task_delete_internal(TaskHandle_t task);
