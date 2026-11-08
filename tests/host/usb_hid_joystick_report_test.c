#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_hid_joystick_report.h"
#include "usb_joystick_fixture.h"

static void write_bits(uint8_t *data, unsigned offset, unsigned bits, uint32_t value)
{
    for (unsigned i = 0; i < bits; i++) {
        if ((value >> i) & 1U) data[(offset + i) / 8] |= 1U << ((offset + i) % 8);
    }
}

int main(void)
{
    solar_os_usb_hid_joystick_layout_t layout;
    solar_os_usb_hid_joystick_sample_t sample;
    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_gamepad_descriptor,
        sizeof(usb_gamepad_descriptor), &layout) == ESP_OK);
    assert(layout.valid && layout.numbered && layout.report_count == 1 && layout.reports[0].bytes == 6);
    uint8_t report[] = {1, 128, 128, 1, 0x80, 8};
    assert(solar_os_usb_hid_joystick_decode(&layout, report, sizeof(report), &sample) == ESP_OK);
    assert(sample.axes_mask == 3 && sample.axes[0] == 0 && sample.axes[1] == 0);
    assert(sample.buttons_mask == 0xffff && sample.buttons == 0x8001);
    assert(sample.directions_mask == 15 && sample.directions == 0);
    report[1] = 0; report[2] = 255; report[5] = 1;
    assert(solar_os_usb_hid_joystick_decode(&layout, report, sizeof(report), &sample) == ESP_OK);
    assert(sample.axes[0] == -32767 && sample.axes[1] == 32767 && sample.directions == 9);
    static const uint8_t directions[] = {1,9,8,10,2,6,4,5,0};
    for (unsigned i = 0; i < 9; i++) {
        report[5] = i;
        assert(solar_os_usb_hid_joystick_decode(&layout, report, sizeof(report), &sample) == ESP_OK);
        assert(sample.directions == directions[i]);
    }
    assert(solar_os_usb_hid_joystick_decode(&layout, report, 5, &sample) == ESP_ERR_INVALID_SIZE);
    const uint8_t unknown[] = {9};
    assert(solar_os_usb_hid_joystick_decode(&layout, unknown, sizeof(unknown), &sample) == ESP_ERR_NOT_FOUND);

    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_split_joystick_descriptor,
        sizeof(usb_split_joystick_descriptor), &layout) == ESP_OK);
    assert(layout.report_count == 2);
    const uint8_t axes[] = {1,0,0x80,0xff,0x7f}, buttons[] = {2,5}, consumer[] = {3,1};
    assert(solar_os_usb_hid_joystick_decode(&layout, axes, sizeof(axes), &sample) == ESP_OK);
    assert(sample.axes_mask == 3 && sample.buttons_mask == 0 && sample.axes[0] == -32767 && sample.axes[1] == 32767);
    assert(solar_os_usb_hid_joystick_decode(&layout, buttons, sizeof(buttons), &sample) == ESP_OK);
    assert(sample.axes_mask == 0 && sample.buttons_mask == 255 && sample.buttons == 5);
    assert(solar_os_usb_hid_joystick_decode(&layout, consumer, sizeof(consumer), &sample) == ESP_ERR_NOT_FOUND);

    /* Six unsigned/signed axes, 32-bit range arithmetic, packed button fields. */
    const uint8_t six_axes[] = {
        0x05,1, 0x09,4, 0xa1,1, 0x19,0x30, 0x29,0x35,
        0x15,0, 0x27,0xff,0xff,0xff,0xff, 0x75,32, 0x95,6, 0x81,2, 0xc0,
    };
    assert(solar_os_usb_hid_joystick_parse_descriptor(six_axes, sizeof(six_axes), &layout) == ESP_OK);
    assert(!layout.numbered && layout.reports[0].bytes == 24);
    uint8_t wide[24] = {0};
    for (unsigned i = 0; i < 6; i++) write_bits(wide, i * 32, 32, i % 3 == 0 ? 0 : i % 3 == 1 ? 0x80000000 : 0xffffffff);
    assert(solar_os_usb_hid_joystick_decode(&layout, wide, sizeof(wide), &sample) == ESP_OK);
    for (unsigned i = 0; i < 6; i++) assert(sample.axes[i] == (i % 3 == 0 ? -32767 : i % 3 == 1 ? 0 : 32767));
    assert(sample.axes_mask == 63);
    const uint8_t dpad[] = {
        0x05,1, 0x09,5, 0xa1,1, 0x19,0x90, 0x29,0x93,
        0x15,0, 0x25,1, 0x75,1, 0x95,4, 0x81,2,
        0x75,4, 0x95,1, 0x81,1, 0xc0,
    };
    assert(solar_os_usb_hid_joystick_parse_descriptor(dpad, sizeof(dpad), &layout) == ESP_OK);
    const uint8_t diagonal[] = {5}; /* Up + Right HID usages. */
    assert(solar_os_usb_hid_joystick_decode(&layout, diagonal, sizeof(diagonal), &sample) == ESP_OK);
    assert(sample.directions_mask == 15 && sample.directions == 9);

    const uint8_t four_way[] = {
        0x05,1, 0x09,4, 0xa1,1, 0x09,0x39, 0x15,1, 0x25,4,
        0x75,4, 0x95,1, 0x81,0x42, 0x75,4, 0x81,1, 0xc0,
    };
    assert(solar_os_usb_hid_joystick_parse_descriptor(four_way, sizeof(four_way), &layout) == ESP_OK);
    static const uint8_t four_directions[] = {0,1,8,2,4,0};
    for (uint8_t value = 0; value < 6; value++) {
        assert(solar_os_usb_hid_joystick_decode(&layout, &value, 1, &sample) == ESP_OK);
        assert(sample.directions == four_directions[value]);
    }
    const uint8_t two_controllers[] = {
        0x05,1, 0x09,4, 0xa1,1, 0x85,1, 0x09,0x30,
        0x15,0, 0x26,0xff,0, 0x75,8, 0x95,1, 0x81,2, 0xc0,
        0x09,4, 0xa1,1, 0x85,2, 0x09,0x31, 0x81,2, 0xc0,
    };
    assert(solar_os_usb_hid_joystick_parse_descriptor(two_controllers, sizeof(two_controllers), &layout) == ESP_OK);
    assert(layout.report_count == 1 && layout.axes[0].field.bits == 8 && layout.axes[1].field.bits == 0);
    const uint8_t second_controller[] = {2,255};
    assert(solar_os_usb_hid_joystick_decode(&layout, second_controller, sizeof(second_controller), &sample) == ESP_ERR_NOT_FOUND);

    assert(solar_os_usb_hid_joystick_parse_descriptor(usb_gamepad_descriptor,
        sizeof(usb_gamepad_descriptor) - 1, &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(!layout.valid);
    /* Valid non-game controller collections must not be misclassified. */
    uint8_t other[sizeof(usb_gamepad_descriptor)];
    memcpy(other, usb_gamepad_descriptor, sizeof(other)); other[3] = 6;
    assert(solar_os_usb_hid_joystick_parse_descriptor(other, sizeof(other), &layout) == ESP_ERR_NOT_SUPPORTED);
    /* Mutations exercise malformed usages, ranges, sizes and report IDs. */
    for (size_t offset = 0; offset < sizeof(other); offset++) {
        for (unsigned value = 0; value < 256; value++) {
            memcpy(other, usb_gamepad_descriptor, sizeof(other)); other[offset] = value;
            if (solar_os_usb_hid_joystick_parse_descriptor(other, sizeof(other), &layout) == ESP_OK) {
                const uint8_t zeros[64] = {0};
                (void)solar_os_usb_hid_joystick_decode(&layout, zeros, sizeof(zeros), &sample);
            }
        }
    }
    puts("USB HID joystick descriptor tests: ok");
    return 0;
}
