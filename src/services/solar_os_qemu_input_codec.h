#pragma once

#include "solar_os_input.h"
#include "solar_os_qemu_input_protocol.h"

typedef struct {
    uint32_t type, a, b, c, d;
} solar_os_qemu_input_event_t;

typedef struct {
    solar_os_input_source_t keyboard, touch, mouse;
    uint8_t keys[256];
    bool pressed[256];
    bool caps_lock;
    uint8_t touch_buttons, mouse_buttons;
    int16_t touch_x, touch_y;
    bool pointer_published;
    uint16_t width, height;
} solar_os_qemu_input_state_t;

esp_err_t solar_os_qemu_input_release_all(solar_os_qemu_input_state_t *state);
/* On queue pressure, retry the same event until accepted or release_all(). */
esp_err_t solar_os_qemu_input_process(solar_os_qemu_input_state_t *state,
                                      const solar_os_qemu_input_event_t *event);
