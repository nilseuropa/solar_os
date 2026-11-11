#include "solar_os_board_usb.h"
#include "solar_os_board.h"

#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
#include "driver/gpio.h"

#ifndef SOLAR_OS_BOARD_USB_VBUS_ACTIVE_LEVEL
#define SOLAR_OS_BOARD_USB_VBUS_ACTIVE_LEVEL 1
#endif
#endif

esp_err_t solar_os_board_usb_set_vbus(bool enabled)
{
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    const gpio_num_t pin = SOLAR_OS_BOARD_PIN_USB_VBUS_EN;
    esp_err_t ret = gpio_hold_dis(pin);
    if (ret != ESP_OK) return ret;
    ret = gpio_sleep_sel_dis(pin);
    if (ret != ESP_OK) return ret;
    const int active = SOLAR_OS_BOARD_USB_VBUS_ACTIVE_LEVEL ? 1 : 0;
    /* Set the latch before enabling the output, preserving the boot-off state. */
    ret = gpio_set_level(pin, enabled ? active : !active);
    if (ret != ESP_OK) return ret;
    return gpio_set_direction(pin, GPIO_MODE_OUTPUT);
#else
    (void)enabled;
    return ESP_OK;
#endif
}
