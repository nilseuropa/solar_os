#pragma once
#include "../../epd_scan_stubs/freertos/FreeRTOS.h"
#define tskNO_AFFINITY (-1)
typedef void (*TaskFunction_t)(void *);

#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
