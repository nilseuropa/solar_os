#pragma once

#include <freertos/FreeRTOS.h>
#include <stdbool.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    pthread_t owner;
    unsigned count;
    bool mutex;
    bool recursive;
    unsigned depth;
} StaticSemaphore_t;
typedef StaticSemaphore_t *SemaphoreHandle_t;
#define portMAX_DELAY 0xffffffffU

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage);
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutexStatic(StaticSemaphore_t *storage);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
#define xSemaphoreTakeRecursive xSemaphoreTake
#define xSemaphoreGiveRecursive xSemaphoreGive
