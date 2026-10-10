#pragma once
#include <stdint.h>
typedef int portMUX_TYPE;
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t StackType_t;
void test_enter_critical(void);
void test_leave_critical(void);
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock), test_enter_critical())
#define portEXIT_CRITICAL(lock) ((void)(lock), test_leave_critical())
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdPASS 1
#define tskNO_AFFINITY -1
