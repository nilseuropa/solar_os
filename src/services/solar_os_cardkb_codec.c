#include "solar_os_cardkb_codec.h"

#include <stddef.h>

#include "solar_os_keys.h"

bool solar_os_cardkb_decode(uint8_t value, uint8_t *key)
{
    if (key == NULL || value == 0) {
        return false;
    }

    switch (value) {
    case 180U:
        *key = SOLAR_OS_KEY_LEFT;
        return true;
    case 181U:
        *key = SOLAR_OS_KEY_UP;
        return true;
    case 182U:
        *key = SOLAR_OS_KEY_DOWN;
        return true;
    case 183U:
        *key = SOLAR_OS_KEY_RIGHT;
        return true;
    case '\r':
        *key = SOLAR_OS_KEY_ENTER;
        return true;
    case 0x7fU:
        *key = SOLAR_OS_KEY_DELETE;
        return true;
    default:
        break;
    }

    /* Values 128..175 are CardKB-specific Fn combinations. They do not map
     * to the SolarOS logical-key range, which starts at 0x80. */
    if (value > 0x7fU) {
        return false;
    }
    *key = value;
    return true;
}
