#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_ABI_VERSION 1U

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *target;
    const char *firmware_version;
    int (*write_utf8)(const char *text, size_t text_len);
    /* Optional append-only extension. Check struct_size before accessing. */
    const void *(*get_service)(const char *name, uint32_t abi_version,
                               uint32_t minimum_struct_size);
} solar_os_native_host_api_v1_t;

/* The ELF module imports this single versioned symbol from SolarOS. */
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void);

#ifdef __cplusplus
}
#endif
