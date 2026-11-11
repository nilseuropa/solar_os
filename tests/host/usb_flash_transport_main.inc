int main(void) {
    uint8_t tx_buffer[FLASH_USB_TX_BLOCK];
    flash_loader_port_t port = {.uart = "uart0", .serial = {.index = 2}, .usb = true,
        .usb_reset = true, .boot_pin = -1, .reset_pin = -1, .usb_tx = tx_buffer};
    assert(flash_port_init(&port.base) == ESP_LOADER_SUCCESS);
    flash_port_enter_bootloader(&port.base);
    assert(line_count == 3 && lines[0] == 1 && lines[1] == 2 && lines[2] == 0 && delays == 200 && !gpio_calls);
    line_count = delays = 0; port.usb_jtag = true;
    flash_port_enter_bootloader(&port.base);
    assert(line_count == 4 && lines[0] == 2 && lines[1] == 3 && lines[2] == 1 && lines[3] == 0 && delays == 200);
    line_count = 0; flash_port_reset_target(&port.base); assert(line_count == 2 && lines[0] == 1 && !lines[1]);
    assert(flash_port_change_rate(&port.base, 460800) == ESP_LOADER_SUCCESS && rate == 460800);
    uint8_t data[4] = {1,2,3,4};
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && usb_reads == 4 && !uart_reads);
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && !usb_writes && !uart_writes);
    read_error = ESP_ERR_TIMEOUT;
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_ERROR_TIMEOUT && port.io_error == ESP_OK);
    assert(usb_writes == 4 && !port.usb_tx_used && usb_sent_size == 4); /* Partial writes complete before reading. */
    read_error = ESP_ERR_INVALID_STATE;
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_ERROR_FAIL && port.io_error == ESP_ERR_INVALID_STATE);
    port.io_error = ESP_OK; read_error = ESP_OK; control_error = ESP_ERR_NOT_SUPPORTED;
    line_count = 0; flash_port_enter_bootloader(&port.base);
    assert(port.io_error == control_error && !line_count);
    assert(flash_port_write(&port.base, data, 1, 100) == ESP_LOADER_ERROR_FAIL);
    port.io_error = ESP_OK; control_error = ESP_OK; read_error = ESP_OK;

    /* The SDK's many tiny SLIP fragments must fit its three-second RAM deadline
     * when every USB transaction costs a 10 ms scheduler tick. */
    uint8_t packet[6405];
    for (size_t i = 0; i < sizeof(packet); i++) packet[i] = (uint8_t)(i * 37);
    usb_writes = 0; usb_sent_size = 0; usb_write_limit = SIZE_MAX;
    flash_port_start_timer(&port.base, 3000);
    for (size_t offset = 0; offset < sizeof(packet);) {
        size_t count = offset % 2 + 1;
        if (count > sizeof(packet) - offset) count = sizeof(packet) - offset;
        assert(flash_port_write(&port.base, packet + offset, count,
            flash_port_remaining(&port.base)) == ESP_LOADER_SUCCESS);
        offset += count;
    }
    assert(port.usb_tx_used && usb_writes == sizeof(packet) / FLASH_USB_TX_BLOCK);
    assert(flash_port_read(&port.base, data, 1, flash_port_remaining(&port.base)) == ESP_LOADER_SUCCESS);
    assert(usb_writes == 13 && usb_sent_size == sizeof(packet) && !memcmp(usb_sent, packet, sizeof(packet)));
    assert(flash_port_remaining(&port.base) > 2500);

    usb_sent_size = 0;
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS);
    assert(flash_port_change_rate(&port.base, 230400) == ESP_LOADER_SUCCESS && rate == 230400 && !port.usb_tx_used);
    assert(usb_sent_size == 4);
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS);
    usb_write_error = ESP_ERR_TIMEOUT;
    assert(flash_port_read(&port.base, data, 1, 100) == ESP_LOADER_ERROR_TIMEOUT && port.io_error == ESP_ERR_TIMEOUT);
    flash_port_deinit(&port.base); assert(!port.usb_tx_used);
    port.io_error = ESP_OK; usb_write_error = ESP_OK; usb_write_zero = true;
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS);
    assert(flash_port_flush_usb(&port) == ESP_LOADER_ERROR_FAIL && port.io_error == ESP_FAIL);
    flash_port_deinit(&port.base); usb_write_zero = false;

    port.io_error = ESP_OK; control_error = ESP_OK; port.usb = port.usb_reset = false; port.boot_pin = 0; port.reset_pin = 1;
    flash_port_enter_bootloader(&port.base); assert(gpio_calls == 4);
    flash_port_reset_target(&port.base); assert(gpio_calls == 6);
    assert(flash_port_change_rate(&port.base, 115200) == ESP_LOADER_SUCCESS && rate == 115200);
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && uart_reads == 4);
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && uart_writes == 4);
}
