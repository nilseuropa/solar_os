#include "solar_os_vector.h"

#include <stdbool.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_engines.h"

#ifndef SOLAR_OS_PACKAGE_SERVICE_ENGINES
#define SOLAR_OS_PACKAGE_SERVICE_ENGINES 0
#endif

static bool vector_begin(const char *label, solar_os_engine_token_t *token)
{
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
    return solar_os_engine_begin("cpu", "vector", label, token) == ESP_OK;
#else
    (void)label;
    (void)token;
    return false;
#endif
}

static void vector_end(solar_os_engine_token_t *token, uint64_t units)
{
#if SOLAR_OS_PACKAGE_SERVICE_ENGINES
    if (token != NULL && token->active) {
        (void)solar_os_engine_end(token, units);
    }
#else
    (void)token;
    (void)units;
#endif
}

void solar_os_vector_fill_rgb565_be(uint8_t *dst, uint16_t rgb565, size_t pixels)
{
    if (dst == NULL || pixels == 0) {
        return;
    }

    solar_os_engine_token_t token = {0};
    const bool tracked = vector_begin("rgb565.fill", &token);

    const uint8_t hi = (uint8_t)(rgb565 >> 8);
    const uint8_t lo = (uint8_t)(rgb565 & 0xff);
    if (hi == lo) {
        memset(dst, hi, pixels * 2U);
    } else {
        for (size_t i = 0; i < pixels; i++) {
            dst[(i * 2U) + 0] = hi;
            dst[(i * 2U) + 1] = lo;
        }
    }

    if (tracked) {
        vector_end(&token, pixels);
    }
}

void solar_os_vector_expand_1bpp_to_rgb565_be(uint8_t *dst,
                                              const uint8_t *columns,
                                              unsigned bit,
                                              uint16_t zero_rgb565,
                                              uint16_t one_rgb565,
                                              size_t pixels)
{
    if (dst == NULL || columns == NULL || pixels == 0 || bit >= 8) {
        return;
    }

    solar_os_engine_token_t token = {0};
    const bool tracked = vector_begin("rgb565.1bpp", &token);

    const uint8_t zero_hi = (uint8_t)(zero_rgb565 >> 8);
    const uint8_t zero_lo = (uint8_t)(zero_rgb565 & 0xff);
    const uint8_t one_hi = (uint8_t)(one_rgb565 >> 8);
    const uint8_t one_lo = (uint8_t)(one_rgb565 & 0xff);
    const uint8_t mask = (uint8_t)(1U << bit);

    for (size_t i = 0; i < pixels; i++) {
        const bool one = (columns[i] & mask) != 0;
        dst[(i * 2U) + 0] = one ? one_hi : zero_hi;
        dst[(i * 2U) + 1] = one ? one_lo : zero_lo;
    }

    if (tracked) {
        vector_end(&token, pixels);
    }
}
