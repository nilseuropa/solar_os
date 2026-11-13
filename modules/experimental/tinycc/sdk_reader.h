#pragma once
#include "solar_os_native_storage_abi.h"

typedef int (*solar_os_cc_sdk_read_fn)(void *,uint32_t,void *,uint32_t);
int solar_os_cc_sdk_extract(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,const char *header,void *buffer,uint32_t capacity,uint32_t *bytes);
int solar_os_cc_sdk_read(const solar_os_native_storage_api_v1_t *storage,
    const char *name,void *buffer,uint32_t capacity,uint32_t *bytes);
