#pragma once
#include "esp_err.h"
#include "solar_os_native_service_abi.h"

static inline int solar_os_native_result_value(esp_err_t error)
{
    switch(error) {
    case ESP_OK: return SOLAR_OS_NATIVE_OK;
    case ESP_ERR_INVALID_ARG: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ESP_ERR_INVALID_STATE: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    case ESP_ERR_NO_MEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ESP_ERR_NOT_FOUND: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    case ESP_ERR_TIMEOUT: return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    case ESP_ERR_INVALID_SIZE: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
#ifdef ESP_ERR_NOT_ALLOWED
    case ESP_ERR_NOT_ALLOWED: return SOLAR_OS_NATIVE_ERROR_PERMISSION;
#endif
    default: return SOLAR_OS_NATIVE_ERROR_FAILED;
    }
}

#ifdef ESP_PLATFORM
int solar_os_native_error_result(esp_err_t error);
#endif

static inline int solar_os_native_result(esp_err_t error)
{
#ifdef ESP_PLATFORM
    /* Keep successful I/O on the inline path and share error conversion. */
    return error == ESP_OK ? SOLAR_OS_NATIVE_OK : solar_os_native_error_result(error);
#else
    return solar_os_native_result_value(error);
#endif
}

/* Convert module callback results back at the private service boundary. */
static inline esp_err_t solar_os_native_callback_result(int result)
{
    switch (result) {
    case SOLAR_OS_NATIVE_OK: return ESP_OK;
    case SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT: return ESP_ERR_INVALID_ARG;
    case SOLAR_OS_NATIVE_ERROR_INVALID_STATE: case SOLAR_OS_NATIVE_ERROR_BUSY: return ESP_ERR_INVALID_STATE;
    case SOLAR_OS_NATIVE_ERROR_NO_MEMORY: return ESP_ERR_NO_MEM;
    case SOLAR_OS_NATIVE_ERROR_NOT_FOUND: return ESP_ERR_NOT_FOUND;
    case SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED: return ESP_ERR_NOT_SUPPORTED;
    case SOLAR_OS_NATIVE_ERROR_TIMEOUT: return ESP_ERR_TIMEOUT;
    case SOLAR_OS_NATIVE_ERROR_INVALID_SIZE: return ESP_ERR_INVALID_SIZE;
    default: return ESP_FAIL;
    }
}
