#pragma once
#include "freertos/task.h"
typedef enum { SOLAR_OS_TASK_ROLE_BACKGROUND } solar_os_task_role_t;
BaseType_t solar_os_task_create_pinned_internal(TaskFunction_t, const char *, uint32_t,
    void *, UBaseType_t, TaskHandle_t *, BaseType_t, solar_os_task_role_t);
void solar_os_task_delete_internal(TaskHandle_t);
