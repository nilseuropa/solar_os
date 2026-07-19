#include "solar_os_pins.h"

#include <stdint.h>

#include "solar_os_board.h"
#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_GPIO
static const solar_os_board_pin_t board_pins[] = SOLAR_OS_BOARD_GPIO_SLOTS;
#endif

static bool mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

size_t solar_os_pin_count(void)
{
#if SOLAR_OS_BOARD_HAS_GPIO
    return sizeof(board_pins) / sizeof(board_pins[0]);
#else
    return 0;
#endif
}

bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *info)
{
#if SOLAR_OS_BOARD_HAS_GPIO
    if (info == NULL || index >= solar_os_pin_count()) {
        return false;
    }

    const solar_os_board_pin_t *pin = &board_pins[index];
    *info = (solar_os_pin_info_t) {
        .pin = pin->pin,
        .expansion = mask_contains(SOLAR_OS_BOARD_EXPANSION_GPIO_MASK, pin->pin),
        .direct_gpio = (pin->policy == SOLAR_OS_PIN_POLICY_FREE &&
                        mask_contains(SOLAR_OS_BOARD_USER_GPIO_MASK, pin->pin)) ||
            (pin->policy == SOLAR_OS_PIN_POLICY_RELEASABLE &&
             mask_contains(SOLAR_OS_BOARD_EXPANSION_GPIO_MASK, pin->pin)),
        .policy = pin->policy,
        .role = pin->role,
    };
    return true;
#else
    (void)index;
    (void)info;
    return false;
#endif
}

bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *info)
{
    for (size_t i = 0; i < solar_os_pin_count(); i++) {
        solar_os_pin_info_t candidate;
        if (solar_os_pin_get_info(i, &candidate) && candidate.pin == pin) {
            if (info != NULL) {
                *info = candidate;
            }
            return true;
        }
    }
    return false;
}

bool solar_os_pin_is_expansion(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) && info.expansion;
}

bool solar_os_pin_is_direct_gpio(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) && info.direct_gpio;
}

bool solar_os_pin_is_routable(int pin)
{
    solar_os_pin_info_t info;
    return solar_os_pin_get_info_by_pin(pin, &info) &&
        info.expansion &&
        info.policy != SOLAR_OS_PIN_POLICY_FIXED;
}

const char *solar_os_pin_policy_name(solar_os_pin_policy_t policy)
{
    switch (policy) {
    case SOLAR_OS_PIN_POLICY_FIXED:
        return "fixed";
    case SOLAR_OS_PIN_POLICY_RELEASABLE:
        return "releasable";
    case SOLAR_OS_PIN_POLICY_FREE:
        return "free";
    default:
        return "unknown";
    }
}
