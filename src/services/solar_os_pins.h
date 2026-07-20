#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "solar_os_pin_types.h"

typedef struct {
    int pin;
    bool expansion;
    bool direct_gpio;
    solar_os_pin_policy_t policy;
    const char *role;
} solar_os_pin_info_t;

size_t solar_os_pin_count(void);
bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *info);
bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info);
bool solar_os_pin_is_expansion(int pin);
bool solar_os_pin_is_direct_gpio(int pin);
bool solar_os_pin_is_routable(int pin);
const char *solar_os_pin_policy_name(solar_os_pin_policy_t policy);
