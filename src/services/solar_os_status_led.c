#include "solar_os_status_led.h"

#include "solar_os_board_caps.h"

#if SOLAR_OS_BOARD_HAS_STATUS_LED
#include "gpio_port.h"
#include "solar_os_board.h"
#endif

#ifndef SOLAR_OS_BOARD_STATUS_LED_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_STATUS_LED_ACTIVE_LEVEL 1
#endif

static bool status_led_initialized;
static bool status_led_on;

static bool status_led_level(bool on)
{
    return SOLAR_OS_BOARD_STATUS_LED_ACTIVE_LEVEL ? on : !on;
}

esp_err_t solar_os_status_led_init(void)
{
#if !SOLAR_OS_BOARD_HAS_STATUS_LED
    return ESP_ERR_NOT_SUPPORTED;
#else
    const esp_err_t err =
        gpio_port_configure(SOLAR_OS_BOARD_PIN_STATUS_LED, GPIO_PORT_MODE_OUTPUT, GPIO_PORT_PULL_NONE);
    if (err != ESP_OK) {
        return err;
    }
    status_led_initialized = true;
    return solar_os_status_led_set(false);
#endif
}

esp_err_t solar_os_status_led_set(bool on)
{
#if !SOLAR_OS_BOARD_HAS_STATUS_LED
    (void)on;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!status_led_initialized) {
        const esp_err_t init_err =
            gpio_port_configure(SOLAR_OS_BOARD_PIN_STATUS_LED, GPIO_PORT_MODE_OUTPUT, GPIO_PORT_PULL_NONE);
        if (init_err != ESP_OK) {
            return init_err;
        }
        status_led_initialized = true;
    }

    const esp_err_t err = gpio_port_write(SOLAR_OS_BOARD_PIN_STATUS_LED, status_led_level(on));
    if (err == ESP_OK) {
        status_led_on = on;
    }
    return err;
#endif
}

esp_err_t solar_os_status_led_toggle(bool *on_after)
{
#if !SOLAR_OS_BOARD_HAS_STATUS_LED
    if (on_after != NULL) {
        *on_after = false;
    }
    return ESP_ERR_NOT_SUPPORTED;
#else
    const esp_err_t err = solar_os_status_led_set(!status_led_on);
    if (err == ESP_OK && on_after != NULL) {
        *on_after = status_led_on;
    }
    return err;
#endif
}

esp_err_t solar_os_status_led_get(bool *on)
{
    if (on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if !SOLAR_OS_BOARD_HAS_STATUS_LED
    *on = false;
    return ESP_ERR_NOT_SUPPORTED;
#else
    *on = status_led_on;
    return ESP_OK;
#endif
}
