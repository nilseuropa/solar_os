#pragma once

typedef enum {
    SOLAR_OS_PIN_POLICY_FIXED = 0,
    SOLAR_OS_PIN_POLICY_RELEASABLE,
    SOLAR_OS_PIN_POLICY_FREE,
} solar_os_pin_policy_t;

typedef struct {
    int pin;
    solar_os_pin_policy_t policy;
    const char *role;
} solar_os_board_pin_t;
