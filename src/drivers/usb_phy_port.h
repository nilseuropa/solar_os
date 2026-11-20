#pragma once

#include "driver/gpio.h"

/* ESP32-S3 internal PHY pins. Call only after both USB controllers have stopped
 * and while their pins are owned by the caller. gpio_config selects GPIO mux,
 * disables Serial/JTAG USB pads, and removes USB/GPIO pulls through IDF. */
static inline esp_err_t usb_phy_port_release_pins(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = (1ULL << 19) | (1ULL << 20),
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&config);
}
