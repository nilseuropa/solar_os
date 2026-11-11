int main(void) {
    device = (usb_device_desc_t){0x2341, 0x8036}; address = 27;
    new_device((void *)1);
    assert(count == 1 && found[0].address == 27 && found[0].interface == 0 && found[0].kind == SOLAR_OS_USB_SERIAL_ACM);
    device = (usb_device_desc_t){0x303a, 0x1001}; address = 3;
    raw[14] = 0xff; raw[15] = 0; raw[23] = 2; raw[24] = 2; raw[32] = 10;
    new_device((void *)1);
    assert(count == 2 && found[1].kind == SOLAR_OS_USB_SERIAL_JTAG && found[1].interface == 1);
    raw[23] = 10; raw[24] = 0; raw[32] = 3;
    /* Same VID/PID on separate hub ports retain distinct addresses. */
    device = (usb_device_desc_t){0x10c4, 0xea60}; raw[14] = 0xff; raw[15] = 0;
    address = 4; new_device((void *)1); address = 5; new_device((void *)1);
    assert(count == 4 && found[2].kind == SOLAR_OS_USB_SERIAL_CP210X && found[2].address == 4 && found[3].address == 5);
    device = (usb_device_desc_t){0x1a86, 0x7523}; new_device((void *)1);
    assert(count == 5 && found[4].kind == SOLAR_OS_USB_SERIAL_CH34X);
    /* An arbitrary vendor-specific interface must not be captured as serial. */
    device = (usb_device_desc_t){0x1234, 0x5678}; new_device((void *)1); assert(count == 5);
    /* Alternate settings and malformed descriptor lengths are ignored. */
    device = (usb_device_desc_t){0x10c4, 0xea60}; raw[12] = 1;
    new_device((void *)1); assert(count == 5);
    raw[12] = 0; raw[9] = 0; new_device((void *)1); assert(count == 5);
}
