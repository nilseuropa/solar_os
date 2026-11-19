#pragma once
#include <stdatomic.h>
#include "solar_os_native_storage_abi.h"
#include "solar_os_storage.h"

typedef struct {
    solar_os_native_storage_api_v1_t api;
    char cwd[SOLAR_OS_STORAGE_PATH_MAX];
    solar_os_native_cancel_fn cancel;
    void *cancel_user;
    atomic_bool stopped;
} solar_os_native_storage_context_t;

esp_err_t solar_os_native_storage_init(solar_os_native_storage_context_t *context,
    const char *cwd, solar_os_native_cancel_fn cancel, void *cancel_user);
const void *solar_os_native_storage_get_service(const char *name, uint32_t abi,
    uint32_t minimum_size, void *context);
