#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

#define SOLAR_OS_NATIVE_LIBC_SERVICE "libc"
#define SOLAR_OS_NATIVE_LIBC_ABI 1U

/* Standard C helpers for native providers. va_list follows the validated
 * target's C calling convention. These helpers retain no caller pointers. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*format)(char *buffer, size_t size, const char *format, va_list args);
    float (*parse_float)(const char *text, char **end);
    double (*floor_value)(double value);
    float (*exp_float)(float value);
    float (*floor_float)(float value);
    float (*mod_float)(float value, float divisor);
    float (*log_float)(float value);
    float (*pow_float)(float value, float exponent);
    float (*sqrt_float)(float value);
} solar_os_native_libc_api_v1_t;
