#pragma once
#include <stdbool.h>
#include "freertos/task.h"
#define SOLAR_OS_TASK_ROLE_FOREGROUND 1
#define SOLAR_OS_TASK_STOP_WAIT_MS 1000
BaseType_t solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name,
    unsigned stack, void *user, unsigned priority, TaskHandle_t *task, int core, int role);
void solar_os_task_delete_internal(TaskHandle_t);
bool solar_os_task_wait_done(TaskHandle_t, volatile bool *, uint32_t);
