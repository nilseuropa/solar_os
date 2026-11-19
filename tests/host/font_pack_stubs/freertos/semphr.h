#pragma once

#include "../../freertos/semphr.h"

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutex;
    return &mutex;
}
