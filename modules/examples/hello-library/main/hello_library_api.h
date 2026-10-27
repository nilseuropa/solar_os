#pragma once
#include <stdint.h>

#define HELLO_LIBRARY_ABI 1U
typedef struct {
    uint32_t abi_version, struct_size;
    /* Unsigned addition modulo 2^32. No mutable state or allocation. */
    uint32_t (*add)(uint32_t left, uint32_t right);
} hello_library_api_v1_t;
