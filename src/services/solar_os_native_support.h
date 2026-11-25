#pragma once
#include "esp_err.h"
#include "solar_os_native_service_abi.h"

static inline int solar_os_native_result(esp_err_t error)
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
