#pragma once
#include <stdbool.h>
#include "freertos/task.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdPASS 1
#define pdFAIL 0
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY -1
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000U
typedef enum { SOLAR_OS_TASK_ROLE_BACKGROUND } solar_os_task_role_t;
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t, const char *,
    uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t, solar_os_task_role_t);
void solar_os_task_delete_internal(TaskHandle_t);
bool solar_os_task_wait_done(TaskHandle_t, volatile bool *, uint32_t);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
BaseType_t xTaskNotifyGive(TaskHandle_t);
