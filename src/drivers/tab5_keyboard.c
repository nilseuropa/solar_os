#include "tab5_keyboard.h"

#include <stddef.h>
#include "solar_os_buses.h"

/* Register protocol and FIFO behavior checked against M5Stack's internal FW:
 * https://github.com/m5stack/M5Tab5-Keyboard-Internal-FW
 * Core/User/i2c/user_i2c_callback.c and fifo/user_event_fifo.c.
 * No upstream implementation is incorporated. */
static esp_err_t write_byte(const char *bus, uint8_t address, uint8_t reg, uint8_t value)
{
    return solar_os_bus_i2c_write_reg(bus, address, reg, &value, 1U);
}

esp_err_t solar_os_tab5_keyboard_decode(uint8_t event, uint16_t *physical,
                                      bool *pressed)
{
    if (physical == NULL || pressed == NULL) return ESP_ERR_INVALID_ARG;
    if (event == 0xffU) return ESP_ERR_NOT_FOUND;
    const unsigned row = (event >> 4U) & 7U, col = event & 15U;
    if (row >= SOLAR_OS_TAB5_KEYBOARD_ROWS || col >= SOLAR_OS_TAB5_KEYBOARD_COLS)
        return ESP_ERR_INVALID_RESPONSE;
    *physical = (uint16_t)(1U + row * SOLAR_OS_TAB5_KEYBOARD_COLS + col);
    *pressed = (event & 0x80U) != 0U;
    return ESP_OK;
}

esp_err_t solar_os_tab5_keyboard_clear(const char *bus, uint8_t address)
{
    esp_err_t err = write_byte(bus, address, SOLAR_OS_TAB5_KEYBOARD_REG_COUNT, 0U);
    if (err == ESP_OK)
        err = write_byte(bus, address, SOLAR_OS_TAB5_KEYBOARD_REG_INT_STAT, 0U);
    return err;
}

esp_err_t solar_os_tab5_keyboard_configure(const char *bus, uint8_t address)
{
    esp_err_t err = write_byte(bus, address, SOLAR_OS_TAB5_KEYBOARD_REG_MODE, 0U);
    /* Enable only Normal-mode INT. Polling also works without wiring INT. */
    if (err == ESP_OK)
        err = write_byte(bus, address, SOLAR_OS_TAB5_KEYBOARD_REG_INT_CFG, 1U);
    if (err == ESP_OK) err = solar_os_tab5_keyboard_clear(bus, address);
    return err;
}

esp_err_t solar_os_tab5_keyboard_read(const char *bus, uint8_t address,
    uint8_t events[SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH], unsigned *count)
{
    if (events == NULL || count == NULL) return ESP_ERR_INVALID_ARG;
    *count = 0U;
    uint8_t mode, pending;
    esp_err_t err = solar_os_bus_i2c_read_reg(bus, address,
        SOLAR_OS_TAB5_KEYBOARD_REG_MODE, &mode, 1U);
    if (err != ESP_OK) return err;
    if (mode != 0U) return ESP_ERR_INVALID_STATE;
    /* Never gate queue reads on INT_STAT: acknowledging INT can race with a
     * new event, and the firmware keeps INT low until explicitly cleared. */
    for (unsigned i = 0; i < SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH; i++) {
        err = solar_os_bus_i2c_read_reg(bus, address,
            SOLAR_OS_TAB5_KEYBOARD_REG_COUNT, &pending, 1U);
        if (err != ESP_OK) return err;
        if (pending >= SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH) return ESP_ERR_INVALID_RESPONSE;
        if (pending == 0U) break;
        uint8_t event;
        err = solar_os_bus_i2c_read_reg(bus, address,
            SOLAR_OS_TAB5_KEYBOARD_REG_EVENT, &event, 1U);
        if (err != ESP_OK) return err;
        uint16_t physical;
        bool pressed;
        err = solar_os_tab5_keyboard_decode(event, &physical, &pressed);
        if (err == ESP_ERR_NOT_FOUND) break;
        if (err != ESP_OK) return err;
        events[(*count)++] = event;
    }
    err = solar_os_bus_i2c_read_reg(bus, address,
        SOLAR_OS_TAB5_KEYBOARD_REG_COUNT, &pending, 1U);
    if (err != ESP_OK) return err;
    if (pending >= SOLAR_OS_TAB5_KEYBOARD_FIFO_DEPTH) return ESP_ERR_INVALID_RESPONSE;
    return write_byte(bus, address, SOLAR_OS_TAB5_KEYBOARD_REG_INT_STAT, 0U);
}
