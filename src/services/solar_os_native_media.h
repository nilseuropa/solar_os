#pragma once
#include <stdint.h>
const void *solar_os_native_media_get_service(const char *name, uint32_t abi_version,
                                             uint32_t minimum_struct_size);
