#pragma once
#include "solar_os_native_library_abi.h"
int solar_os_native_library_acquire(const char *id,uint32_t abi,uint32_t minimum,uint32_t *lease,const void **api);
int solar_os_native_library_release(uint32_t lease);
