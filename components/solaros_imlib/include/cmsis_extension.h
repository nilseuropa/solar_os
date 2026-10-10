/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: MIT
 */

#ifndef ESP_VISION_IMLIB_CMSIS_EXTENSION_H
#define ESP_VISION_IMLIB_CMSIS_EXTENSION_H

#include <stdbool.h>

#include "cmsis_compiler.h"

static uint32_t esp_vision_cmsis_ge_flags __attribute__((unused));

__STATIC_FORCEINLINE void esp_vision_cmsis_set_ge8(uint32_t flags) {
    esp_vision_cmsis_ge_flags = flags & 0xfU;
}

__STATIC_FORCEINLINE void esp_vision_cmsis_set_ge16(bool lo_ge, bool hi_ge) {
    esp_vision_cmsis_ge_flags = (lo_ge ? 0x3U : 0U) | (hi_ge ? 0xcU : 0U);
}

__STATIC_FORCEINLINE int32_t __SSAT_ASR(int32_t value, uint32_t sat, uint32_t shift) {
    return __SSAT(value >> (shift & 31U), sat);
}

__STATIC_FORCEINLINE uint32_t __USAT_ASR(int32_t value, uint32_t sat, uint32_t shift) {
    return __USAT(value >> (shift & 31U), sat);
}

__STATIC_FORCEINLINE int32_t __SSAT16(int32_t value, uint32_t sat) {
    int32_t hi = (int16_t) (value >> 16);
    int32_t lo = (int16_t) value;
    hi = __SSAT(hi, sat);
    lo = __SSAT(lo, sat);
    return (hi << 16) | (lo & 0xffff);
}

__STATIC_FORCEINLINE uint32_t __USAT16(int32_t value, uint32_t sat) {
    int32_t hi = (int16_t) (value >> 16);
    int32_t lo = (int16_t) value;
    uint32_t uhi = __USAT(hi, sat);
    uint32_t ulo = __USAT(lo, sat);
    return (uhi << 16) | (ulo & 0xffff);
}

__STATIC_FORCEINLINE uint32_t __SADD8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    uint32_t ge = 0;
    for (uint32_t i = 0; i < 4; i++) {
        int32_t sum = (int8_t) (value1 >> (i * 8)) + (int8_t) (value2 >> (i * 8));
        result |= ((uint32_t) ((uint8_t) sum)) << (i * 8);
        if (sum >= 0) {
            ge |= 1U << i;
        }
    }
    esp_vision_cmsis_set_ge8(ge);
    return result;
}

__STATIC_FORCEINLINE uint32_t __SSUB8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    uint32_t ge = 0;
    for (uint32_t i = 0; i < 4; i++) {
        int32_t diff = (int8_t) (value1 >> (i * 8)) - (int8_t) (value2 >> (i * 8));
        result |= ((uint32_t) ((uint8_t) diff)) << (i * 8);
        if (diff >= 0) {
            ge |= 1U << i;
        }
    }
    esp_vision_cmsis_set_ge8(ge);
    return result;
}

__STATIC_FORCEINLINE uint32_t __USUB8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    uint32_t ge = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t a = (value1 >> (i * 8)) & 0xffU;
        uint32_t b = (value2 >> (i * 8)) & 0xffU;
        result |= ((a - b) & 0xffU) << (i * 8);
        if (a >= b) {
            ge |= 1U << i;
        }
    }
    esp_vision_cmsis_set_ge8(ge);
    return result;
}

__STATIC_FORCEINLINE uint32_t __SHADD8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; i++) {
        int32_t sum = (int8_t) (value1 >> (i * 8)) + (int8_t) (value2 >> (i * 8));
        result |= ((uint32_t) ((uint8_t) (sum >> 1))) << (i * 8);
    }
    return result;
}

__STATIC_FORCEINLINE uint32_t __UHADD8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t sum = ((value1 >> (i * 8)) & 0xffU) + ((value2 >> (i * 8)) & 0xffU);
        result |= ((sum >> 1) & 0xffU) << (i * 8);
    }
    return result;
}

__STATIC_FORCEINLINE uint32_t __UQADD8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t sum = ((value1 >> (i * 8)) & 0xffU) + ((value2 >> (i * 8)) & 0xffU);
        result |= (sum > 0xffU ? 0xffU : sum) << (i * 8);
    }
    return result;
}

__STATIC_FORCEINLINE uint32_t __UQSUB8(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t a = (value1 >> (i * 8)) & 0xffU;
        uint32_t b = (value2 >> (i * 8)) & 0xffU;
        result |= (a > b ? (a - b) : 0U) << (i * 8);
    }
    return result;
}

__STATIC_FORCEINLINE uint32_t __SADD16(uint32_t value1, uint32_t value2) {
    int32_t lo = (int16_t) value1 + (int16_t) value2;
    int32_t hi = (int16_t) (value1 >> 16) + (int16_t) (value2 >> 16);
    esp_vision_cmsis_set_ge16(lo >= 0, hi >= 0);
    return ((uint32_t) ((uint16_t) hi) << 16) | (uint16_t) lo;
}

__STATIC_FORCEINLINE uint32_t __SSUB16(uint32_t value1, uint32_t value2) {
    int32_t lo = (int16_t) value1 - (int16_t) value2;
    int32_t hi = (int16_t) (value1 >> 16) - (int16_t) (value2 >> 16);
    esp_vision_cmsis_set_ge16(lo >= 0, hi >= 0);
    return ((uint32_t) ((uint16_t) hi) << 16) | (uint16_t) lo;
}

__STATIC_FORCEINLINE uint32_t __USUB16(uint32_t value1, uint32_t value2) {
    uint32_t lo1 = value1 & 0xffffU;
    uint32_t lo2 = value2 & 0xffffU;
    uint32_t hi1 = (value1 >> 16) & 0xffffU;
    uint32_t hi2 = (value2 >> 16) & 0xffffU;
    esp_vision_cmsis_set_ge16(lo1 >= lo2, hi1 >= hi2);
    return (((hi1 - hi2) & 0xffffU) << 16) | ((lo1 - lo2) & 0xffffU);
}

__STATIC_FORCEINLINE uint32_t __UHADD16(uint32_t value1, uint32_t value2) {
    uint32_t lo = ((value1 & 0xffffU) + (value2 & 0xffffU)) >> 1;
    uint32_t hi = (((value1 >> 16) & 0xffffU) + ((value2 >> 16) & 0xffffU)) >> 1;
    return (hi << 16) | (lo & 0xffffU);
}

__STATIC_FORCEINLINE uint32_t __SHADD16(uint32_t value1, uint32_t value2) {
    int32_t lo = ((int16_t) value1 + (int16_t) value2) >> 1;
    int32_t hi = ((int16_t) (value1 >> 16) + (int16_t) (value2 >> 16)) >> 1;
    return ((uint32_t) ((uint16_t) hi) << 16) | (uint16_t) lo;
}

__STATIC_FORCEINLINE uint32_t __UXTB16(uint32_t value) {
    return (value & 0x000000ffU) | ((value & 0x00ff0000U) >> 8);
}

__STATIC_FORCEINLINE uint32_t __UXTH(uint32_t value) {
    return value & 0xffffU;
}

__STATIC_FORCEINLINE uint32_t __UXTB16_RORn(uint32_t value, uint32_t rotate) {
    return __UXTB16(__ROR(value, rotate));
}

__STATIC_FORCEINLINE uint32_t __SXTB16(uint32_t value) {
    int32_t lo = (int8_t) value;
    int32_t hi = (int8_t) (value >> 16);
    return ((uint32_t) ((uint16_t) hi) << 16) | ((uint16_t) lo);
}

__STATIC_FORCEINLINE int32_t __SXTH(uint32_t value) {
    return (int16_t) value;
}

__STATIC_FORCEINLINE uint32_t __SEL(uint32_t value1, uint32_t value2) {
    uint32_t result = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t mask = 0xffU << (i * 8);
        result |= (esp_vision_cmsis_ge_flags & (1U << i)) ? (value1 & mask) : (value2 & mask);
    }
    return result;
}

__STATIC_FORCEINLINE uint32_t __QADD16(uint32_t value1, uint32_t value2) {
    int32_t hi = (int16_t) (value1 >> 16) + (int16_t) (value2 >> 16);
    int32_t lo = (int16_t) value1 + (int16_t) value2;
    hi = __SSAT(hi, 16);
    lo = __SSAT(lo, 16);
    return ((uint32_t) ((uint16_t) hi) << 16) | (uint16_t) lo;
}

__STATIC_FORCEINLINE uint32_t __QSUB16(uint32_t value1, uint32_t value2) {
    int32_t hi = (int16_t) (value1 >> 16) - (int16_t) (value2 >> 16);
    int32_t lo = (int16_t) value1 - (int16_t) value2;
    hi = __SSAT(hi, 16);
    lo = __SSAT(lo, 16);
    return ((uint32_t) ((uint16_t) hi) << 16) | (uint16_t) lo;
}

__STATIC_FORCEINLINE uint32_t __PKHBT(uint32_t value1, uint32_t value2, uint32_t shift) {
    return (value1 & 0x0000ffffU) | ((value2 << shift) & 0xffff0000U);
}

__STATIC_FORCEINLINE uint32_t __PKHTB(uint32_t value1, uint32_t value2, uint32_t shift) {
    return (value1 & 0xffff0000U) | ((value2 >> shift) & 0x0000ffffU);
}

__STATIC_FORCEINLINE int32_t __SMLAD(uint32_t value1, uint32_t value2, int32_t acc) {
    int32_t a0 = (int16_t) value1;
    int32_t a1 = (int16_t) (value1 >> 16);
    int32_t b0 = (int16_t) value2;
    int32_t b1 = (int16_t) (value2 >> 16);
    return acc + (a0 * b0) + (a1 * b1);
}

__STATIC_FORCEINLINE int32_t __SMLADX(uint32_t value1, uint32_t value2, int32_t acc) {
    int32_t a0 = (int16_t) value1;
    int32_t a1 = (int16_t) (value1 >> 16);
    int32_t b0 = (int16_t) value2;
    int32_t b1 = (int16_t) (value2 >> 16);
    return acc + (a0 * b1) + (a1 * b0);
}

__STATIC_FORCEINLINE int32_t __SMUAD(uint32_t value1, uint32_t value2) {
    return __SMLAD(value1, value2, 0);
}

__STATIC_FORCEINLINE int32_t __SMUADX(uint32_t value1, uint32_t value2) {
    return __SMLADX(value1, value2, 0);
}

__STATIC_FORCEINLINE uint32_t __USAD8(uint32_t value1, uint32_t value2) {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) {
        int a = (value1 >> (i * 8)) & 0xff;
        int b = (value2 >> (i * 8)) & 0xff;
        sum += (uint32_t) ((a > b) ? (a - b) : (b - a));
    }
    return sum;
}

__STATIC_FORCEINLINE uint32_t __USADA8(uint32_t value1, uint32_t value2, uint32_t acc) {
    return acc + __USAD8(value1, value2);
}

#endif /* ESP_VISION_IMLIB_CMSIS_EXTENSION_H */
