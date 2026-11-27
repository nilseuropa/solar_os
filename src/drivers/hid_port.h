#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum {
    HID_PORT_REPORT_KEYBOARD = 1,
    HID_PORT_REPORT_MOUSE = 2,
    HID_PORT_REPORT_GAMEPAD = 3,
};

typedef struct __attribute__((packed)) {
    uint8_t modifier;
    uint8_t reserved;
    uint8_t keycode[6];
} hid_port_keyboard_report_t;

typedef struct __attribute__((packed)) {
    uint8_t buttons;
    int8_t x;
    int8_t y;
    int8_t wheel;
    int8_t pan;
} hid_port_mouse_report_t;

typedef struct __attribute__((packed)) {
    int8_t x;
    int8_t y;
    int8_t z;
    int8_t rz;
    int8_t rx;
    int8_t ry;
    uint8_t hat;
    uint32_t buttons;
} hid_port_gamepad_report_t;

bool hid_port_available(void);
bool hid_port_is_connected(void);
esp_err_t hid_port_send_report(uint8_t report_id, const void *report, size_t report_len);
