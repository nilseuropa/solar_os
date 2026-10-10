#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_TAB5_KEYBOARD_ADDRESS 0x6dU
#define SOLAR_OS_TAB5_KEYBOARD_ROWS 5U
#define SOLAR_OS_TAB5_KEYBOARD_COLS 14U
#define SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH 32U
#define SOLAR_OS_TAB5_KEYBOARD_REG_INT_CFG 0x00U
#define SOLAR_OS_TAB5_KEYBOARD_REG_INT_STAT 0x01U
#define SOLAR_OS_TAB5_KEYBOARD_REG_COUNT 0x02U
#define SOLAR_OS_TAB5_KEYBOARD_REG_MODE 0x10U
#define SOLAR_OS_TAB5_KEYBOARD_REG_EVENT 0x20U

/* Normal-mode events: bit 7 is press, bits 6:4 row, bits 3:0 column.
 * Physical IDs are 1 + row * 14 + column; 0xff means an empty FIFO. */
esp_err_t solar_os_tab5_keyboard_decode(uint8_t event, uint16_t *physical,
                                      bool *pressed);
esp_err_t solar_os_tab5_keyboard_configure(const char *bus, uint8_t address);
esp_err_t solar_os_tab5_keyboard_clear(const char *bus, uint8_t address);

/* Stage at most one FIFO before publishing. Saturation is treated as lost
 * state because the firmware drops new events without an overflow flag. */
esp_err_t solar_os_tab5_keyboard_read(const char *bus, uint8_t address,
    uint8_t events[SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH], unsigned *count);
