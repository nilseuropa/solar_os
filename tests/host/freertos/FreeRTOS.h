#pragma once

#include <stdint.h>

typedef int portMUX_TYPE;
typedef uint32_t TickType_t;

#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
