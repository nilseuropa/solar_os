#pragma once
#include "solar_os_cc_abi.h"
#include "solar_os_native_abi.h"
#include "solar_os_native_libc_abi.h"

int solar_os_cc_sdk_command(const solar_os_native_host_api_v1_t *host,
    const solar_os_native_libc_api_v1_t *libc_api, const solar_os_cc_api_v1_t *compiler,
    const solar_os_native_memory_api_v1_t *memory, int argc, char **argv);
