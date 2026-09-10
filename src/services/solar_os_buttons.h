#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

typedef enum {
    SOLAR_OS_BUTTON_PULL_NONE = 0,
    SOLAR_OS_BUTTON_PULL_UP,
    SOLAR_OS_BUTTON_PULL_DOWN,
} solar_os_button_pull_t;

typedef struct {
    gpio_num_t pin;
    const char *name;
    uint8_t key;
    bool active_low;
    bool emit_on_release;
    solar_os_button_pull_t pull;
    /* For directional rollers, reject a short horizontal impulse immediately
     * after a vertical movement. Zero leaves normal button behavior intact. */
    uint16_t horizontal_guard_after_vertical_ms;
} solar_os_button_def_t;

typedef struct {
    gpio_num_t pin;
    const char *name;
    uint8_t key;
    bool raw_pressed;
    bool stable_pressed;
    uint32_t raw_transitions;
    uint32_t stable_transitions;
    uint32_t suppressed_releases;
} solar_os_button_debug_info_t;

esp_err_t solar_os_buttons_init(void);
void solar_os_buttons_poll(void);
size_t solar_os_buttons_read_chars(char *buffer, size_t buffer_len);
size_t solar_os_buttons_count(void);
bool solar_os_buttons_debug_get(size_t index, solar_os_button_debug_info_t *info);
