#pragma once
#include "solar_os_native_service_abi.h"

enum {
    SOLAR_OS_NATIVE_HANDLE_SHA256 = 1,
    SOLAR_OS_NATIVE_HANDLE_RANDOM,
    SOLAR_OS_NATIVE_HANDLE_PORT,
    SOLAR_OS_NATIVE_HANDLE_FILE,
    SOLAR_OS_NATIVE_HANDLE_DIRECTORY,
    SOLAR_OS_NATIVE_HANDLE_NET,
    SOLAR_OS_NATIVE_HANDLE_HTTP,
    SOLAR_OS_NATIVE_HANDLE_HTTP_RESPONSE,
    SOLAR_OS_NATIVE_HANDLE_WIFI_LEASE,
    SOLAR_OS_NATIVE_HANDLE_SSH,
    SOLAR_OS_NATIVE_HANDLE_SCP,
    SOLAR_OS_NATIVE_HANDLE_SFTPSYNC,
    SOLAR_OS_NATIVE_HANDLE_SFTP,
    SOLAR_OS_NATIVE_HANDLE_STREAM,
    SOLAR_OS_NATIVE_HANDLE_PARAMETER_CLAIM,
    SOLAR_OS_NATIVE_HANDLE_RESOURCES,
    SOLAR_OS_NATIVE_HANDLE_GPIO,
    SOLAR_OS_NATIVE_HANDLE_PWM,
    SOLAR_OS_NATIVE_HANDLE_BUS,
    SOLAR_OS_NATIVE_HANDLE_DISK_IMAGE,
    SOLAR_OS_NATIVE_HANDLE_BACKLIGHT_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_HAPTIC_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_CHARGER_PROVIDER,
    SOLAR_OS_NATIVE_HANDLE_DSP_FIR,
    SOLAR_OS_NATIVE_HANDLE_DSP_DECIMATOR,
    SOLAR_OS_NATIVE_HANDLE_DSP_FFT,
};
typedef int (*solar_os_native_handle_destroy_fn)(void *state);
int solar_os_native_handle_create(uint32_t kind, void *state,
    solar_os_native_handle_destroy_fn destroy, solar_os_native_handle_t *out);
void *solar_os_native_handle_acquire(solar_os_native_handle_t id, uint32_t kind);
void solar_os_native_handle_release(solar_os_native_handle_t id);
int solar_os_native_handle_close(solar_os_native_handle_t id);
int solar_os_native_handle_close_kind(solar_os_native_handle_t id, uint32_t kind);
uint32_t solar_os_native_handle_count(void);
