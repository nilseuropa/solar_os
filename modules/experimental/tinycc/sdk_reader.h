#pragma once
#include "solar_os_native_storage_abi.h"
#include "solar_os_native_sdk.h"

typedef struct {
    char name[96];
    uint32_t offset, bytes; /* Absolute file offset, not an allocated ELF address. */
} solar_os_cc_sdk_entry_t;
typedef struct {
    char version[32];
    uint32_t count;
    solar_os_cc_sdk_entry_t entries[SOLAR_OS_NATIVE_SDK_FILES_MAX];
} solar_os_cc_sdk_index_t;

typedef int (*solar_os_cc_sdk_read_fn)(void *,uint32_t,void *,uint32_t);
int solar_os_cc_sdk_extract(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,const char *header,void *buffer,uint32_t capacity,uint32_t *bytes);
int solar_os_cc_sdk_index(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,solar_os_cc_sdk_index_t *index);
int solar_os_cc_sdk_library_index(const solar_os_native_storage_api_v1_t *storage,
    const char *path,const char *module_id,solar_os_cc_sdk_index_t *index);
int solar_os_cc_sdk_read(const solar_os_native_storage_api_v1_t *storage,
    const char *name,void *buffer,uint32_t capacity,uint32_t *bytes);
