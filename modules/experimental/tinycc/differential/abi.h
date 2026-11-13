#pragma once
#include <stdint.h>

/* Test-only GCC/TinyCC boundary. All calls use the supported scalar32 ABI. */
typedef uint32_t (*diff_callback_t)(uint32_t, uint32_t);
typedef uint32_t (*diff_callback12_t)(uint32_t,uint32_t,uint32_t,uint32_t,
    uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef struct {
    uint32_t (*mix8)(uint32_t, uint32_t, uint32_t, uint32_t,
                     uint32_t, uint32_t, uint32_t, uint32_t);
    int32_t (*narrow)(int8_t, uint8_t, int16_t, uint16_t);
    uint32_t (*callback)(diff_callback_t, uint32_t, uint32_t);
    uint32_t (*variadic)(unsigned, ...);
    int8_t (*signed_byte)(uint32_t);
    uint8_t (*unsigned_byte)(uint32_t);
    int16_t (*signed_half)(uint32_t);
    uint16_t (*unsigned_half)(uint32_t);
    uint32_t (*callback12)(diff_callback12_t,uint32_t,uint32_t);
} diff_api_t;

#define DIFF_WORDS 32
typedef uint32_t (*diff_probe_t)(uint32_t, uint32_t, uint32_t *, const diff_api_t *);
