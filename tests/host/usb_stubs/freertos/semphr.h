#pragma once

#include "../../freertos/semphr.h"
#include <stdlib.h>

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(StaticSemaphore_t));
}
static inline void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }
