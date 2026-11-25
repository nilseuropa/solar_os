#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_hid_types_v1.h"

esp_err_t solar_os_hid_init(void);
void solar_os_hid_get_status(solar_os_hid_status_t *status);

esp_err_t solar_os_hid_keyboard_press(const uint16_t *keys, size_t key_count);
esp_err_t solar_os_hid_keyboard_release(const uint16_t *keys, size_t key_count);
esp_err_t solar_os_hid_keyboard_release_all(void);

esp_err_t solar_os_hid_mouse_move(int32_t x, int32_t y);
esp_err_t solar_os_hid_mouse_button(uint8_t button, bool pressed);

esp_err_t solar_os_hid_gamepad_axis(solar_os_hid_axis_t axis, int16_t value);
esp_err_t solar_os_hid_gamepad_button(uint8_t button, bool pressed);
esp_err_t solar_os_hid_gamepad_hat(solar_os_hid_hat_t hat);
esp_err_t solar_os_hid_gamepad_send(void);

/* Neutralize every report type. Intended for script/job lifecycle cleanup. */
void solar_os_hid_release_all(void);
