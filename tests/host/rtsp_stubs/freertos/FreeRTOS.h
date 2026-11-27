#pragma once
#include <stdint.h>
#ifdef RTSP_STUBS_PTHREAD_CRITICAL
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
#endif
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define tskIDLE_PRIORITY 0U
#define tskNO_AFFINITY -1
#define portMAX_DELAY UINT32_MAX
