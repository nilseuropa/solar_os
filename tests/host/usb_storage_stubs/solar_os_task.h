#pragma once
#include <stdint.h>
#define pdPASS 1
#define SOLAR_OS_TASK_ROLE_SYSTEM 0
int solar_os_task_create_pinned(void (*task)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, void *handle, int core, int role);
void solar_os_task_delete(void *handle);
