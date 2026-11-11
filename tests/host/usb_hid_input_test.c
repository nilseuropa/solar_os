#define main input_service_tests
#include "input_test.c"
#undef main

#include "solar_os_usb_hid_input.h"
#include "usb_receiver_fixture.h"
#include "usb_joystick_fixture.h"

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

    assert(solar_os_usb_hid_input_open(&mouse, "usb-mouse1", 2) == ESP_OK);
    assert(solar_os_usb_hid_mouse_parse_descriptor(usb_receiver_mouse_descriptor,
        sizeof(usb_receiver_mouse_descriptor), &mouse.mouse_layout) == ESP_OK);
    const uint8_t receiver_motion[] = {1, 1, 0xed, 0xff, 0, 0};
    assert(solar_os_usb_hid_input_report(&mouse, receiver_motion, sizeof(receiver_motion)) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(pointer.buttons == 1 && pointer.delta_x == -19 && pointer.delta_y == -1);
    assert(pointer.action == SOLAR_OS_INPUT_POINTER_PRESS);
    const uint8_t consumer[] = {2, 0, 0}, system[] = {3, 0};
    assert(solar_os_usb_hid_input_report(&mouse, consumer, sizeof(consumer)) == ESP_OK);
    assert(solar_os_usb_hid_input_report(&mouse, system, sizeof(system)) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 0 && mouse.buttons == 1);
    const uint8_t release[] = {1, 0, 0, 0, 0, 0};
    assert(solar_os_usb_hid_input_report(&mouse, release, sizeof(release)) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(pointer.buttons == 0 && pointer.action == SOLAR_OS_INPUT_POINTER_RELEASE);
    assert(solar_os_usb_hid_input_report(&mouse, receiver_motion, sizeof(receiver_motion)) == ESP_OK);
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(solar_os_usb_hid_input_report(&mouse, receiver_motion, 3) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_input_read_pointer_events(&pointer, 1) == 1);
    assert(pointer.buttons == 0 && pointer.action == SOLAR_OS_INPUT_POINTER_RELEASE);
    solar_os_usb_hid_input_close(&mouse);
    assert(solar_os_input_source_count() == 0);

    solar_os_usb_hid_input_t joystick;
    assert(solar_os_usb_hid_input_open(&joystick, "usb-joy0", SOLAR_OS_USB_HID_JOYSTICK) == ESP_OK);
    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_gamepad_descriptor,
        sizeof(usb_gamepad_descriptor), &joystick.joystick_layout) == ESP_OK);
    solar_os_input_source_info_t info;
    assert(solar_os_input_source_get_info(joystick.source, &info));
    assert(info.source_class == SOLAR_OS_INPUT_SOURCE_JOYSTICK);
    assert(info.capabilities == (SOLAR_OS_INPUT_CAP_KEY_EVENTS | SOLAR_OS_INPUT_CAP_AXIS_EVENTS));
    uint8_t gamepad[] = {1,128,128,1,0x80,1}; /* Buttons 1/16, Up+Right. */
    assert(solar_os_usb_hid_input_report(&joystick, gamepad, sizeof(gamepad)) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 4);
    assert(keys[0].physical_key == 1 && keys[0].key == 0 && keys[0].usage == 0);
    assert(keys[1].physical_key == 16 && keys[1].action == SOLAR_OS_INPUT_KEY_PRESS);
    assert(keys[2].key == SOLAR_OS_KEY_UP && keys[3].key == SOLAR_OS_KEY_RIGHT);
    solar_os_input_axis_event_t axes[6];
    assert(solar_os_input_read_axis_events(axes, 6) == 2);
    assert(axes[0].axis == SOLAR_OS_INPUT_AXIS_X && axes[0].value == 0);
    assert(axes[1].axis == SOLAR_OS_INPUT_AXIS_Y && axes[1].value == 0);
    assert(solar_os_usb_hid_input_report(&joystick, gamepad, sizeof(gamepad)) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 0 && solar_os_input_read_axis_events(axes, 6) == 0);
    gamepad[1] = 0; gamepad[2] = 255; gamepad[3] = 2; gamepad[4] = 0; gamepad[5] = 5;
    assert(solar_os_usb_hid_input_report(&joystick, gamepad, sizeof(gamepad)) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 7);
    for (unsigned i = 0; i < 7; i++) assert(keys[i].action == (i < 4 ? SOLAR_OS_INPUT_KEY_RELEASE : SOLAR_OS_INPUT_KEY_PRESS));
    assert(solar_os_input_read_axis_events(axes, 6) == 2);
    assert(axes[0].value == -32767 && axes[1].value == 32767);
    const solar_os_input_source_t retired = joystick.source;
    solar_os_usb_hid_input_close(&joystick);
    assert(solar_os_input_read_events(keys, 16) == 3);
    for (unsigned i = 0; i < 3; i++) assert(keys[i].action == SOLAR_OS_INPUT_KEY_RELEASE);
    /* Neutral axes survive closing and prevent ID reuse until consumed. */
    assert(solar_os_input_joystick_source_open("replacement-joy", &replacement) == ESP_OK);
    assert(replacement != retired);
    assert(solar_os_input_read_axis_events(axes, 6) == 2);
    assert(axes[0].source == retired && axes[0].value == 0 && axes[0].delta == 32767);
    assert(axes[1].source == retired && axes[1].value == 0 && axes[1].delta == -32767);
    solar_os_input_source_close(replacement);

    assert(solar_os_usb_hid_input_open(&joystick, "usb-joy0", SOLAR_OS_USB_HID_JOYSTICK) == ESP_OK);
    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_split_joystick_descriptor,
        sizeof(usb_split_joystick_descriptor), &joystick.joystick_layout) == ESP_OK);
    const uint8_t joystick_buttons[] = {2,1}, joystick_axes[] = {1,0,0x80,0xff,0x7f}, media_report[] = {3,1};
    assert(solar_os_usb_hid_input_report(&joystick, joystick_buttons, sizeof(joystick_buttons)) == ESP_OK);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].physical_key == 1);
    assert(solar_os_usb_hid_input_report(&joystick, joystick_axes, sizeof(joystick_axes)) == ESP_OK);
    assert(solar_os_usb_hid_input_report(&joystick, media_report, sizeof(media_report)) == ESP_OK);
    assert(solar_os_input_read_axis_events(axes, 6) == 2);
    assert(solar_os_input_read_events(keys, 16) == 0 && joystick.joystick_buttons == 1);
    assert(solar_os_usb_hid_input_report(&joystick, joystick_axes, 3) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_input_read_events(keys, 16) == 1 && keys[0].action == SOLAR_OS_INPUT_KEY_RELEASE);
    assert(solar_os_input_read_axis_events(axes, 6) == 2 && axes[0].value == 0 && axes[1].value == 0);
    solar_os_usb_hid_input_close(&joystick);
    assert(solar_os_input_source_count() == 0);

    /* Queue pressure cannot discard final controller button/direction releases. */
    assert(solar_os_usb_hid_input_open(&joystick, "usb-joy0", SOLAR_OS_USB_HID_JOYSTICK) == ESP_OK);
    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_gamepad_descriptor,
        sizeof(usb_gamepad_descriptor), &joystick.joystick_layout) == ESP_OK);
    uint8_t held[] = {1,128,128,0xff,0xff,1};
    assert(solar_os_usb_hid_input_report(&joystick, held, sizeof(held)) == ESP_OK);
    while (solar_os_input_read_events(keys, 16)) { }
    assert(solar_os_input_read_axis_events(axes, 6) == 2);
    assert(solar_os_input_source_open("pressure", &pressure) == ESP_OK);
    for (unsigned i = 0; i < 32; i++) assert(solar_os_input_write_char(pressure, 'x') == ESP_OK);
    const solar_os_input_source_t controller_source = joystick.source;
    solar_os_usb_hid_input_close(&joystick);
    releases = 0;
    while ((count = solar_os_input_read_events(keys, 16)) != 0) {
        for (size_t i = 0; i < count; i++) {
            if (keys[i].source == controller_source && keys[i].action == SOLAR_OS_INPUT_KEY_RELEASE) releases++;
        }
    }
    assert(releases == 18);
    solar_os_input_source_close(pressure);
    assert(solar_os_input_source_count() == 0);
    puts("USB HID input tests: ok");
    return 0;
}
