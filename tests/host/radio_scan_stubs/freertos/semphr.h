#pragma once
#include "../../epd_scan_stubs/freertos/semphr.h"
static inline void vSemaphoreDelete(SemaphoreHandle_t s)
{
    assert(pthread_cond_destroy(&s->wake) == 0);
    assert(pthread_mutex_destroy(&s->lock) == 0);
}
