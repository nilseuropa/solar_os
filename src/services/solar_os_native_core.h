#pragma once
#include "solar_os_native_service_abi.h"
const void *solar_os_native_core_get_service(const char *name, uint32_t abi, uint32_t minimum);
int solar_os_native_port_claim(const char *name, const char *owner, solar_os_native_handle_t *out);
int solar_os_native_port_close(solar_os_native_handle_t id);
