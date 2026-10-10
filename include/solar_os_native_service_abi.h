#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Stable results for the image/media/vision/inference service tables. */
typedef enum {
    SOLAR_OS_NATIVE_OK = 0,
    SOLAR_OS_NATIVE_ERROR_FAILED = -1,
    SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT = -2,
    SOLAR_OS_NATIVE_ERROR_INVALID_STATE = -3, /* Includes admission contention. */
    SOLAR_OS_NATIVE_ERROR_NO_MEMORY = -4,
    SOLAR_OS_NATIVE_ERROR_NOT_FOUND = -5,
    SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED = -6,
    SOLAR_OS_NATIVE_ERROR_TIMEOUT = -7, /* Includes cooperative cancellation. */
    SOLAR_OS_NATIVE_ERROR_INVALID_SIZE = -10,
} solar_os_native_service_result_v1_t;

/* Called synchronously on the calling task, never after the call returns.
 * Media/inference clients retain this callback until destroyed: keep its code
 * and user data alive. Close clients before unloading their module. */
typedef bool (*solar_os_native_cancel_fn)(void *user);

#define SOLAR_OS_NATIVE_MEMORY_SERVICE "memory"
#define SOLAR_OS_NATIVE_MEMORY_ABI 1U
/* Request-owned PSRAM. Allocation failure returns NULL. Release all buffers
 * before module unload. Standard C helpers may read/write these buffers. */
typedef struct {
    uint32_t abi_version, struct_size;
    void *(*alloc)(size_t bytes);
    void (*free)(void *buffer);
} solar_os_native_memory_api_v1_t;
