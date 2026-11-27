#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI 1U

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *id;
    const char *summary;
    uint32_t tick_interval_ms;
    uint32_t tick_deadline_ms;
    int (*start)(int argc, char **argv);
    void (*stop)(void);
    bool (*tick)(uint32_t now_ms);
} solar_os_native_job_descriptor_v1_t;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *target;
    const char *firmware_version;
    int (*register_job)(const solar_os_native_job_descriptor_v1_t *descriptor);
    const void *(*get_service)(const char *name, uint32_t abi_version,
                               uint32_t minimum_struct_size);
} solar_os_native_job_host_api_v1_t;

const solar_os_native_job_host_api_v1_t *solar_os_native_job_host_v1(void);

#ifdef __cplusplus
}
#endif
