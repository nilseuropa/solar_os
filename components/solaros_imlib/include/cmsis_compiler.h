/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: MIT
 */

#ifndef ESP_VISION_IMLIB_CMSIS_COMPILER_H
#define ESP_VISION_IMLIB_CMSIS_COMPILER_H

#include <stdint.h>

#ifndef __ASM
#define __ASM __asm
#endif

#ifndef __INLINE
#define __INLINE inline
#endif

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif

#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))
#endif

#ifndef __NO_RETURN
#define __NO_RETURN __attribute__((noreturn))
#endif

#ifndef __USED
#define __USED __attribute__((used))
#endif

#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#ifndef __PACKED
#define __PACKED __attribute__((packed))
#endif

#ifndef __PACKED_STRUCT
#define __PACKED_STRUCT struct __attribute__((packed))
#endif

#ifndef __PACKED_UNION
#define __PACKED_UNION union __attribute__((packed))
#endif

#ifndef __ALIGNED
#define __ALIGNED(x) __attribute__((aligned(x)))
#endif

__STATIC_FORCEINLINE uint32_t __REV(uint32_t value) {
    return __builtin_bswap32(value);
}

__STATIC_FORCEINLINE uint32_t __REV16(uint32_t value) {
    return ((value & 0x00ff00ffU) << 8) | ((value & 0xff00ff00U) >> 8);
}

__STATIC_FORCEINLINE uint32_t __ROR(uint32_t value, uint32_t shift) {
    shift &= 31U;
    return shift ? ((value >> shift) | (value << (32U - shift))) : value;
}

__STATIC_FORCEINLINE uint32_t __RBIT(uint32_t value) {
    value = ((value & 0x55555555U) << 1) | ((value >> 1) & 0x55555555U);
    value = ((value & 0x33333333U) << 2) | ((value >> 2) & 0x33333333U);
    value = ((value & 0x0f0f0f0fU) << 4) | ((value >> 4) & 0x0f0f0f0fU);
    return __REV(value);
}

__STATIC_FORCEINLINE uint32_t __CLZ(uint32_t value) {
    return value ? (uint32_t) __builtin_clz(value) : 32U;
}

__STATIC_FORCEINLINE int32_t __SSAT(int32_t value, uint32_t sat) {
    if (sat >= 1U && sat <= 32U) {
        const int32_t max = (int32_t) ((1ULL << (sat - 1U)) - 1ULL);
        const int32_t min = -1 - max;
        if (value > max) {
            return max;
        }
        if (value < min) {
            return min;
        }
    }
    return value;
}

__STATIC_FORCEINLINE uint32_t __USAT(int32_t value, uint32_t sat) {
    if (value < 0) {
        return 0;
    }
    if (sat <= 31U) {
        const uint32_t max = (1U << sat) - 1U;
        if ((uint32_t) value > max) {
            return max;
        }
    }
    return (uint32_t) value;
}

#endif /* ESP_VISION_IMLIB_CMSIS_COMPILER_H */
