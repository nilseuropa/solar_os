#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_hid_mouse_report.h"
#include "usb_receiver_fixture.h"

static void put_bits(uint8_t *data, unsigned offset, unsigned bits, uint16_t value)
{
    for (unsigned i = 0; i < bits; i++) {
        if ((value >> i) & 1U) data[(offset + i) / 8] |= 1U << ((offset + i) % 8);
    }
}

int main(void)
{
    solar_os_usb_hid_mouse_layout_t layout;
    uint8_t buttons;
    int16_t x, y;
    assert(solar_os_usb_hid_mouse_parse_descriptor(usb_receiver_mouse_descriptor,
        sizeof(usb_receiver_mouse_descriptor), &layout) == ESP_OK);
    assert(layout.valid && layout.numbered && layout.report_id == 1 && layout.report_bytes == 6);
    assert(layout.buttons[0].offset == 0 && layout.buttons[2].offset == 2);
    assert(layout.x.offset == 8 && layout.y.offset == 16);
    const uint8_t captured[] = {1, 0, 0xed, 0xff, 0, 0};
    assert(solar_os_usb_hid_mouse_decode(&layout, captured, sizeof(captured), &buttons, &x, &y) == ESP_OK);
    assert(buttons == 0 && x == -19 && y == -1);
    const uint8_t extremes[] = {1, 3, 0x7f, 0x80, 0, 0};
    assert(solar_os_usb_hid_mouse_decode(&layout, extremes, sizeof(extremes), &buttons, &x, &y) == ESP_OK);
    assert(buttons == 3 && x == 127 && y == -128);
    assert(solar_os_usb_hid_mouse_decode(&layout, captured, 5, &buttons, &x, &y) == ESP_ERR_INVALID_SIZE);
    const uint8_t media[] = {2, 0, 0};
    assert(solar_os_usb_hid_mouse_decode(&layout, media, sizeof(media), &buttons, &x, &y) == ESP_ERR_NOT_FOUND);

    /* Signed 16-bit relative coordinates at unaligned bit offsets, no report ID. */
    const uint8_t packed[] = {
        0x05,1, 0x09,2, 0xa1,1, 0x05,9, 0x19,1, 0x29,3,
        0x15,0, 0x25,1, 0x75,1, 0x95,3, 0x81,2,
        0x05,1, 0x09,0x30, 0x09,0x31, 0x16,0,0x80,
        0x26,0xff,0x7f, 0x75,16, 0x95,2, 0x81,6, 0xc0,
    };
    assert(solar_os_usb_hid_mouse_parse_descriptor(packed, sizeof(packed), &layout) == ESP_OK);
    assert(!layout.numbered && layout.report_bytes == 5 && layout.x.offset == 3 && layout.y.offset == 19);
    uint8_t bits[5] = {5};
    put_bits(bits, 3, 16, (uint16_t)-32768);
    put_bits(bits, 19, 16, 32767);
    assert(solar_os_usb_hid_mouse_decode(&layout, bits, sizeof(bits), &buttons, &x, &y) == ESP_OK);
    assert(buttons == 5 && x == -32768 && y == 32767);

    /* Push/Pop restores the report ID; unrelated input has its own bit cursor. */
    const uint8_t stacked[] = {
        0x05,1, 0x09,2, 0xa1,1, 0x85,7, 0x05,9, 0x19,1, 0x29,3,
        0x15,0, 0x25,1, 0x75,1, 0x95,3, 0x81,2,
        0x75,5, 0x95,1, 0x81,1,
        0xa4, 0x85,8, 0x06,0,0xff, 0x09,1, 0x75,8, 0x81,2, 0xb4,
        0x05,1, 0x09,0x30, 0x09,0x31, 0x15,0x81, 0x25,0x7f,
        0x75,8, 0x95,2, 0x81,6, 0xc0,
    };
    assert(solar_os_usb_hid_mouse_parse_descriptor(stacked, sizeof(stacked), &layout) == ESP_OK);
    assert(layout.numbered && layout.report_id == 7 && layout.report_bytes == 4);
    assert(layout.x.offset == 8 && layout.y.offset == 16);

    assert(solar_os_usb_hid_mouse_parse_descriptor(packed, sizeof(packed) - 1, &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(!layout.valid);
    const uint8_t truncated[] = {0x27, 1};
    assert(solar_os_usb_hid_mouse_parse_descriptor(truncated, sizeof(truncated), &layout) == ESP_ERR_INVALID_RESPONSE);
    const uint8_t pop[] = {0xb4}, end[] = {0xc0}, zero_id[] = {0x85, 0};
    assert(solar_os_usb_hid_mouse_parse_descriptor(pop, sizeof(pop), &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_usb_hid_mouse_parse_descriptor(end, sizeof(end), &layout) == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_usb_hid_mouse_parse_descriptor(zero_id, sizeof(zero_id), &layout) == ESP_ERR_INVALID_RESPONSE);
    const uint8_t huge[] = {0x75,32, 0x96,0xff,0xff, 0x81,1};
    assert(solar_os_usb_hid_mouse_parse_descriptor(huge, sizeof(huge), &layout) == ESP_ERR_NOT_SUPPORTED);
    uint8_t absolute[sizeof(packed)];
    memcpy(absolute, packed, sizeof(packed));
    absolute[sizeof(packed) - 2] = 2; /* Absolute X/Y is outside relative mouse support. */
    assert(solar_os_usb_hid_mouse_parse_descriptor(absolute, sizeof(absolute), &layout) == ESP_ERR_NOT_SUPPORTED);

    /* Exercise malformed sizes/usages/collections under ASan/UBSan as well. */
    uint8_t mutated[sizeof(usb_receiver_mouse_descriptor)];
    for (size_t i = 0; i < sizeof(mutated); i++) {
        for (unsigned value = 0; value < 256; value++) {
            memcpy(mutated, usb_receiver_mouse_descriptor, sizeof(mutated));
            mutated[i] = (uint8_t)value;
            if (solar_os_usb_hid_mouse_parse_descriptor(mutated, sizeof(mutated), &layout) == ESP_OK) {
                const uint8_t empty[64] = {0};
                (void)solar_os_usb_hid_mouse_decode(&layout, empty, sizeof(empty), &buttons, &x, &y);
            }
        }
    }
    puts("USB HID mouse descriptor tests: ok");
    return 0;
}
