#pragma once
#include <stdint.h>

/* Test-only GCC/TinyCC boundary for the ESP32-S3 windowed ABI. */
typedef uint32_t (*diff_callback_t)(uint32_t, uint32_t);
typedef uint32_t (*diff_callback12_t)(uint32_t,uint32_t,uint32_t,uint32_t,
    uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t,uint32_t);
typedef uint64_t (*diff_wide_callback_t)(uint32_t,uint64_t,uint32_t,uint64_t,uint32_t);
typedef double (*diff_float_callback_t)(double,float,uint64_t,uint32_t,double);
typedef struct {uint32_t a,b,c;} diff_triple_t;
typedef struct {uint32_t words[5];} diff_large_t;
typedef struct {uint32_t words[4];} diff_quad_t;
typedef struct {uint8_t bytes[3];} diff_small_t;
typedef diff_triple_t (*diff_triple_callback_t)(diff_triple_t,uint32_t);
typedef diff_large_t (*diff_large_callback_t)(diff_large_t,uint32_t);
typedef uint64_t (*diff_variadic_callback_t)(unsigned,...);
typedef diff_quad_t (*diff_quad_callback_t)(uint32_t,diff_quad_t,uint32_t);
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
    uint64_t (*wide)(uint32_t,uint64_t,uint32_t,uint64_t,uint32_t);
    uint64_t (*wide_callback)(diff_wide_callback_t,uint32_t,uint32_t);
    uint64_t (*wide_variadic)(unsigned,...);
    diff_triple_t (*triple)(diff_triple_t,uint32_t);
    uint32_t (*triple_callback)(diff_triple_callback_t,uint32_t,uint32_t);
    diff_large_t (*large)(diff_large_t,uint32_t);
    uint32_t (*large_callback)(diff_large_callback_t,uint32_t,uint32_t);
    double (*floating)(double,float,uint64_t,uint32_t,double);
    uint64_t (*variadic_callback)(diff_variadic_callback_t,uint32_t,uint32_t);
    diff_quad_t (*quad)(uint32_t,diff_quad_t,uint32_t);
    uint32_t (*quad_callback)(diff_quad_callback_t,uint32_t,uint32_t);
    diff_small_t (*small)(uint32_t,diff_small_t,uint32_t);
    double (*floating_callback)(diff_float_callback_t,double,float,uint64_t,uint32_t,double);
} diff_api_t;

#define DIFF_WORDS 32
typedef uint32_t (*diff_probe_t)(uint32_t, uint32_t, uint32_t *, const diff_api_t *);
