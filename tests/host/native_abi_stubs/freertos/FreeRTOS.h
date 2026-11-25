#pragma once
#include <pthread.h>
typedef pthread_mutex_t portMUX_TYPE;
#define pdTRUE 1
#define pdFALSE 0
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(lock) pthread_mutex_lock(lock)
#define portEXIT_CRITICAL(lock) pthread_mutex_unlock(lock)
