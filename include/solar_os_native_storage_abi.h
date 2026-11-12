#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_STORAGE_SERVICE "storage"
#define SOLAR_OS_NATIVE_STORAGE_ABI 1U
#define SOLAR_OS_NATIVE_STORAGE_PATH_MAX 160U
#define SOLAR_OS_NATIVE_STORAGE_READ_MAX 65536U
#define SOLAR_OS_NATIVE_STORAGE_REPLACE_MAX (2U * 1024U * 1024U)

/* A shell-owned capability, valid until the native entry returns. Its context
 * captures that shell's working directory and cancellation input. Workers may
 * use it while the entry waits for them; join all workers before returning.
 * No pointers or handles are retained by these calls. The table and context
 * must not be cached by a resident library for use after the caller returns.
 * resolve_path accepts NULL for the working directory. File operations require
 * resolved absolute paths. Replacement preserves the previous file on ordinary
 * write/sync/rename failure and reserves .native.tmp/.native.bak siblings.
 * This is recoverable replacement, not a power-loss atomicity guarantee. */
typedef struct {
    uint32_t abi_version, struct_size;
    void *context;
    int (*resolve_path)(void *context, const char *path, char *resolved, size_t capacity);
    int (*file_size)(void *context, const char *path, uint32_t *bytes);
    int (*read_file)(void *context, const char *path, uint32_t offset,
                     void *buffer, size_t capacity, size_t *read_bytes);
    int (*replace_file)(void *context, const char *path, const void *buffer, size_t bytes);
    bool (*stop_requested)(void *context);
} solar_os_native_storage_api_v1_t;
