#define main input_service_tests
#include "input_test.c"
#undef main

#include "solar_os_usb_hid_input.h"

int main(void)
{
    assert(solar_os_input_init() == ESP_OK);
    assert(solar_os_input_set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_US) == ESP_OK);
    solar_os_usb_hid_input_t keyboard, mouse;
    assert(solar_os_usb_hid_input_open(&keyboard, "usb-kbd0", 1) == ESP_OK);
    solar_os_input_key_event_t keys[16];
    uint8_t report[8] = {2, 0, 4}; /* Shift+A */
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 2);
    assert(keys[0].usage == 0xe1 && keys[0].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(keys[1].usage == 4 && keys[1].key == 'A');
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 0); /* Repeat belongs to input service. */
    report[0] = 0;
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].usage == 0xe1);
    memset(report, 0, sizeof(report));
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1);
    assert(keys[0].usage == 4 && keys[0].key == 'A' && keys[0].action == SOLAR_OS_INPUT_KEY_RELEASE);

    report[2] = report[3] = 5; /* Duplicate usages must not duplicate presses. */
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].usage == 5);
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 0);
    memset(report, 0, sizeof(report));
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].key == 'b');
    report[2] = 5;
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1);
    report[2] = 1; /* ErrorRollOver releases tracked state, never emits usage 1. */
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].action == SOLAR_OS_INPUT_KEY_RELEASE);
    memset(report, 0, sizeof(report));
    report[2] = 6;
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1);
    solar_os_usb_hid_input_close(&keyboard);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(solar_os_input_keyboard_count() == 0);

    assert(solar_os_usb_hid_input_open(&keyboard, "usb-kbd0", 1) == ESP_OK);
    memset(report, 0, sizeof(report)); report[2] = 4;
    assert(solar_os_usb_hid_input_report(&keyboard, report, 8) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1);
    solar_os_input_source_t pressure;
    assert(solar_os_input_source_open("pressure", &pressure) == ESP_OK);
    for (unsigned i = 0; i < 32; i++) assert(solar_os_input_write_char(pressure, 'x') == ESP_OK);
    solar_os_usb_hid_input_close(&keyboard);
    unsigned releases = 0;
    size_t count;
    while ((count = solar_os_input_read_events(keys, 16)) != 0) {
        for (size_t i = 0; i < count; i++) {
            if (keys[i].usage == 4 && keys[i].action == SOLAR_OS_INPUT_KEY_RELEASE) releases++;
        }
    }
    assert(releases == 1);
    solar_os_input_source_close(pressure);

    assert(solar_os_usb_hid_input_open(&mouse, "usb-mouse1", 2) == ESP_OK);
    const uint8_t motion[] = {1, 0xfe, 0x7f};
    assert(solar_os_usb_hid_input_report(&mouse, motion, sizeof(motion)) == ESP_OK);
    solar_os_input_pointer_event_t pointer;
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(pointer.mode == SOLAR_OS_INPUT_POINTER_RELATIVE && pointer.delta_x == -2 && pointer.delta_y == 127);
    assert(pointer.buttons == 1 && pointer.action == SOLAR_OS_INPUT_POINTER_PRESS);
    solar_os_usb_hid_input_close(&mouse);
    /* Closing a source must preserve its final button-release event. */
    solar_os_input_source_t replacement;
    assert(solar_os_input_mouse_source_open("replacement", &replacement) == ESP_OK);
    assert(replacement != pointer.source); /* Retired IDs cannot be reused yet. */
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(pointer.buttons == 0 && pointer.action == SOLAR_OS_INPUT_POINTER_RELEASE);
    solar_os_input_source_close(replacement);
    assert(solar_os_input_source_count() == 0);
    puts("USB HID input tests: ok");
    return 0;
}
