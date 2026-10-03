#pragma once

#include <stddef.h>

#include "esp_err.h"
#include "solar_os_expansion.h"

/* Elecrow STC8H companion-MCU I2C keypad (ThinkNode M9). Board revision 1.0
 * answers at 0x6c and revision 1.1 at 0x6d; the driver accepts either address */
#define SOLAR_OS_STC8H_KEYBOARD_ADDRESS 0x6cU
#define SOLAR_OS_STC8H_KEYBOARD_ALT_ADDRESS 0x6dU

esp_err_t solar_os_stc8h_keyboard_attach(const char *name,
                                         const solar_os_expansion_binding_t *bindings,
                                         size_t binding_count);
esp_err_t solar_os_stc8h_keyboard_detach(const char *name);
