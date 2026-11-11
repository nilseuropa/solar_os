#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY (-1)
#endif
typedef struct fake_keyboard *keyboard_btn_handle_t;
typedef struct { uint8_t output_index, input_index; } keyboard_btn_data_t;
typedef struct {
    int key_change_num;
    uint32_t key_pressed_num, key_release_num;
    keyboard_btn_data_t *key_data, *key_release_data;
} keyboard_btn_report_t;
typedef void (*keyboard_btn_callback_t)(keyboard_btn_handle_t, keyboard_btn_report_t, void *);
enum { KBD_EVENT_PRESSED };
typedef struct { int event; keyboard_btn_callback_t callback; void *user_data; } keyboard_btn_cb_config_t;
typedef struct {
    const int *output_gpios, *input_gpios;
    uint32_t output_gpio_num, input_gpio_num, active_level, debounce_ticks, ticks_interval;
    bool enable_power_save, start_paused;
    int priority, core_id;
} keyboard_btn_config_t;
esp_err_t keyboard_button_create(keyboard_btn_config_t *, keyboard_btn_handle_t *);
esp_err_t keyboard_button_register_cb(keyboard_btn_handle_t, keyboard_btn_cb_config_t, void *);
esp_err_t solar_os_keyboard_button_start(keyboard_btn_handle_t);
esp_err_t keyboard_button_delete(keyboard_btn_handle_t);
