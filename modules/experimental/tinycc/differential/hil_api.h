#pragma once
#include "diff_abi.h"

/* Test-only installed provider: real GCC code and callbacks on the device. */
typedef struct {
    uint32_t abi_version, struct_size;
    const diff_api_t *callbacks;
    uint32_t case_count, vector_count;
    const uint32_t (*inputs)[2];
    const diff_probe_t (*references)[2];
} solar_os_cc_hil_api_t;
