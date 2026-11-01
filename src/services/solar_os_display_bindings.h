#pragma once
#include <string.h>
#include "solar_os_expansion.h"
#include "u8g2.h"

/* Shared adapter grammar; panel selection, power providers and clock limits
 * remain the controller adapter's policy. */
static inline esp_err_t solar_os_display_parse_spi_binding(
    const solar_os_expansion_binding_t *binding, char *bus, size_t bus_size,
    bool *have_bus, bool *have_cs, int *cs_pin)
{
    if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
        if (*have_bus) return ESP_ERR_INVALID_ARG;
    } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) {
        if (*have_cs) return ESP_ERR_INVALID_ARG;
        *have_cs = true;
        *cs_pin = binding->value;
        if (!binding->target[0]) return ESP_OK;
        if (*have_bus && strcmp(bus, binding->target)) return ESP_ERR_INVALID_ARG;
    } else return ESP_ERR_INVALID_ARG;
    if (strnlen(binding->target, sizeof(binding->target)) >= bus_size) return ESP_ERR_INVALID_SIZE;
    strlcpy(bus, binding->target, bus_size);
    *have_bus = true;
    return ESP_OK;
}

static inline esp_err_t solar_os_display_parse_gpio_binding(
    const solar_os_expansion_binding_t *binding,
    int *dc, int *reset, int *busy, int *power)
{
    int *pin = NULL;
    if (!strcmp(binding->role, "dc")) pin = dc;
    else if (!strcmp(binding->role, "reset") || !strcmp(binding->role, "rst")) pin = reset;
    else if (!strcmp(binding->role, "busy")) pin = busy;
    else if (!strcmp(binding->role, "power")) pin = power;
    if (!pin || *pin >= 0 || binding->value < 0) return ESP_ERR_INVALID_ARG;
    *pin = binding->value;
    return ESP_OK;
}

static inline bool solar_os_display_pins_valid(int cs, int dc, int reset, int busy, int power)
{
    const int pins[] = {cs, dc, reset, busy, power};
    for (size_t i = 0; i < 5; ++i) {
        if (i < 4 && pins[i] < 0) return false;
        for (size_t j = i + 1; j < 5; ++j)
            if (pins[i] >= 0 && pins[i] == pins[j]) return false;
    }
    return true;
}

static inline const u8g2_cb_t *solar_os_display_rotation_callback(int rotation)
{
    static const u8g2_cb_t *const rotations[] = {U8G2_R0, U8G2_R1, U8G2_R2, U8G2_R3};
    return rotation >= 0 && rotation < 4 ? rotations[rotation] : NULL;
}
