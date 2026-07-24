#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SOLAR_OS_WORK_NAME_MAX 24U
#define SOLAR_OS_WORK_STACK_BYTES 16384U
#define SOLAR_OS_WORK_HANDLE_INVALID_SLOT UINT8_MAX

typedef void (*solar_os_work_fn)(void *arg);

typedef struct {
    uint32_t generation;
    uint8_t slot;
} solar_os_work_handle_t;

#define SOLAR_OS_WORK_HANDLE_INIT \
    { .generation = 0, .slot = SOLAR_OS_WORK_HANDLE_INVALID_SLOT }

typedef struct {
    bool initialized;
    bool running;
    size_t stack_bytes;
    UBaseType_t stack_high_water;
    size_t queued;
    uint32_t submitted;
    uint32_t completed;
    uint32_t cancelled;
    uint32_t rejected;
    char current[SOLAR_OS_WORK_NAME_MAX];
} solar_os_work_status_t;

/*
 * Reserve the single internal transient-worker stack. PSRAM boards call this
 * during boot so later work cannot fail because the internal heap fragmented.
 * On constrained boards submit() initializes it lazily.
 */
esp_err_t solar_os_work_init(void);

/*
 * Queue work for the shared internal executor. Queue storage is PSRAM-backed
 * when PSRAM is available. Callbacks run serially and must return when their
 * operation or foreground session ends.
 */
esp_err_t solar_os_work_submit(const char *name,
                               solar_os_work_fn callback,
                               void *arg,
                               UBaseType_t priority,
                               solar_os_work_handle_t *handle,
                               TaskHandle_t *task);

/*
 * Cancel work that has not started. Running work must be stopped cooperatively
 * by its owner; the shared executor itself must never be deleted by a client.
 */
esp_err_t solar_os_work_cancel(solar_os_work_handle_t handle);
bool solar_os_work_active(solar_os_work_handle_t handle);
void solar_os_work_get_status(solar_os_work_status_t *status);
