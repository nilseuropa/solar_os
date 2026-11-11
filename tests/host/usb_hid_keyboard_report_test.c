#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_hid_keyboard_report.h"
#include "usb_leonardo_fixture.h"

static void put_bits(uint8_t *data, unsigned offset, uint8_t value)
{
    for (unsigned i = 0; i < 8; i++) {
        if ((value >> i) & 1U) data[(offset + i) / 8] |= 1U << ((offset + i) % 8);
    }
}

int main(void)
{
    solar_os_usb_hid_keyboard_layout_t layout;
    uint8_t normalized[8];
    assert(solar_os_usb_hid_keyboard_parse_descriptor(usb_leonardo_keyboard_descriptor,
        sizeof(usb_leonardo_keyboard_descriptor), &layout) == ESP_OK);
    assert(layout.valid && layout.numbered && layout.report_id == 2 && layout.report_bytes == 9);
    assert(layout.modifiers == 0xff && layout.key_offset == 16 && layout.key_count == 6);
    const uint8_t report[] = {2,0x22,0,4,5,6,7,8,9};
    assert(solar_os_usb_hid_keyboard_decode(&layout, report, sizeof(report), normalized) == ESP_OK);
    assert(!memcmp(normalized, report + 1, 8));
    assert(solar_os_usb_hid_keyboard_decode(&layout, report, 8, normalized) == ESP_ERR_INVALID_SIZE);
    const uint8_t other[] = {3};
    assert(solar_os_usb_hid_keyboard_decode(&layout, other, sizeof(other), normalized) == ESP_ERR_NOT_FOUND);
    assert(solar_os_usb_hid_keyboard_decode(&layout, report, 0, normalized) == ESP_ERR_INVALID_SIZE);
    const uint8_t bad_key[] = {2,0,0,0xff,0,0,0,0,0};
    assert(solar_os_usb_hid_keyboard_decode(&layout, bad_key, sizeof(bad_key), normalized) == ESP_ERR_INVALID_RESPONSE);

    /* The same keyboard without a report ID also works. */
    uint8_t unnumbered[sizeof(usb_leonardo_keyboard_descriptor) - 2];
    memcpy(unnumbered, usb_leonardo_keyboard_descriptor, 6);
    memcpy(unnumbered + 6, usb_leonardo_keyboard_descriptor + 8, sizeof(unnumbered) - 6);
    assert(solar_os_usb_hid_keyboard_parse_descriptor(unnumbered, sizeof(unnumbered), &layout) == ESP_OK);
    assert(!layout.numbered && layout.report_id == 0 && layout.report_bytes == 8);
    assert(solar_os_usb_hid_keyboard_decode(&layout, report + 1, 8, normalized) == ESP_OK);
    assert(!memcmp(normalized, report + 1, 8));

    /* Field offsets come from the descriptor, including unaligned key arrays. */
    const uint8_t packed[] = {
        0x05,1, 0x09,6, 0xa1,1, 0x05,7, 0x19,0xe0, 0x29,0xe7,
        0x15,0, 0x25,1, 0x75,1, 0x95,8, 0x81,2,
        0x75,3, 0x95,1, 0x81,3,
        0x19,0, 0x29,0x65, 0x15,0, 0x25,0x65, 0x75,8, 0x95,2, 0x81,0, 0xc0,
    };
    assert(solar_os_usb_hid_keyboard_parse_descriptor(packed, sizeof(packed), &layout) == ESP_OK);
    assert(layout.key_offset == 11 && layout.key_count == 2 && layout.report_bytes == 4);
    uint8_t bits[4] = {1};
    put_bits(bits, 11, 4); put_bits(bits, 19, 5);
    const uint8_t expected[] = {1,0,4,5,0,0,0,0};
    assert(solar_os_usb_hid_keyboard_decode(&layout, bits, sizeof(bits), normalized) == ESP_OK);
    assert(!memcmp(normalized, expected, sizeof(expected)));

    /* Consumer reports in another collection must not alter keyboard decoding. */
    const uint8_t consumer[] = {
        0x05,0x0c, 0x09,1, 0xa1,1, 0x85,3, 0x09,0xe9,
        0x15,0, 0x25,1, 0x75,1, 0x95,1, 0x81,2, 0xc0,
    };
    uint8_t composite[sizeof(usb_leonardo_keyboard_descriptor) + sizeof(consumer)];
    memcpy(composite, usb_leonardo_keyboard_descriptor, sizeof(usb_leonardo_keyboard_descriptor));
    memcpy(composite + sizeof(usb_leonardo_keyboard_descriptor), consumer, sizeof(consumer));
    assert(solar_os_usb_hid_keyboard_parse_descriptor(composite, sizeof(composite), &layout) == ESP_OK);
    assert(layout.report_id == 2 && layout.report_bytes == 9);
    assert(solar_os_usb_hid_keyboard_decode(&layout, report, sizeof(report), normalized) == ESP_OK);
    assert(!memcmp(normalized, report + 1, 8));

    /* Oversized arrays, bitmap keys, split reports and truncated descriptors fail closed. */
    uint8_t modified[sizeof(usb_leonardo_keyboard_descriptor)];
    memcpy(modified, usb_leonardo_keyboard_descriptor, sizeof(modified));
    modified[31] = 7; /* REPORT_COUNT (key array). */
    assert(solar_os_usb_hid_keyboard_parse_descriptor(modified, sizeof(modified), &layout) == ESP_ERR_NOT_SUPPORTED);
    assert(!layout.valid);
    memcpy(modified, usb_leonardo_keyboard_descriptor, sizeof(modified));
    modified[45] = 2; /* Input (Data,Var,Abs): bitmap, not an array. */
    assert(solar_os_usb_hid_keyboard_parse_descriptor(modified, sizeof(modified), &layout) == ESP_ERR_NOT_SUPPORTED);
    uint8_t split[sizeof(usb_leonardo_keyboard_descriptor) + 2];
    memcpy(split, usb_leonardo_keyboard_descriptor, 30);
    split[30] = 0x85; split[31] = 3;
    memcpy(split + 32, usb_leonardo_keyboard_descriptor + 30, sizeof(split) - 32);
    assert(solar_os_usb_hid_keyboard_parse_descriptor(split, sizeof(split), &layout) == ESP_ERR_NOT_SUPPORTED);
    for (size_t n = 0; n < sizeof(usb_leonardo_keyboard_descriptor); n++) {
        assert(solar_os_usb_hid_keyboard_parse_descriptor(usb_leonardo_keyboard_descriptor, n, &layout) != ESP_OK);
        assert(!layout.valid);
    }
    puts("USB report keyboards: Leonardo, report IDs, arrays, modifiers and malformed layouts: ok");
    return 0;
}
