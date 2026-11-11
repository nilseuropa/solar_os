int main(void) {
    flash_loader_port_t port = {.uart = "uart0", .serial = {.index = 2}, .usb = true,
        .usb_reset = true, .boot_pin = -1, .reset_pin = -1};
    flash_port_enter_bootloader(&port.base);
    assert(line_count == 3 && lines[0] == 1 && lines[1] == 2 && lines[2] == 0 && delays == 200 && !gpio_calls);
    line_count = delays = 0; port.usb_jtag = true;
    flash_port_enter_bootloader(&port.base);
    assert(line_count == 4 && lines[0] == 2 && lines[1] == 3 && lines[2] == 1 && lines[3] == 0 && delays == 200);
    line_count = 0; flash_port_reset_target(&port.base); assert(line_count == 2 && lines[0] == 1 && !lines[1]);
    assert(flash_port_change_rate(&port.base, 460800) == ESP_LOADER_SUCCESS && rate == 460800);
    uint8_t data[4] = {1,2,3,4};
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && usb_reads == 4 && !uart_reads);
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && usb_writes == 4 && !uart_writes);
    read_error = ESP_ERR_TIMEOUT;
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_ERROR_TIMEOUT && port.io_error == ESP_OK);
    read_error = ESP_ERR_INVALID_STATE;
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_ERROR_FAIL && port.io_error == ESP_ERR_INVALID_STATE);
    port.io_error = ESP_OK; read_error = ESP_OK; control_error = ESP_ERR_NOT_SUPPORTED;
    line_count = 0; flash_port_enter_bootloader(&port.base);
    assert(port.io_error == control_error && !line_count);
    assert(flash_port_write(&port.base, data, 1, 100) == ESP_LOADER_ERROR_FAIL);
    port.io_error = ESP_OK; control_error = ESP_OK; port.usb = port.usb_reset = false; port.boot_pin = 0; port.reset_pin = 1;
    flash_port_enter_bootloader(&port.base); assert(gpio_calls == 4);
    flash_port_reset_target(&port.base); assert(gpio_calls == 6);
    assert(flash_port_change_rate(&port.base, 115200) == ESP_LOADER_SUCCESS && rate == 115200);
    assert(flash_port_read(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && uart_reads == 4);
    assert(flash_port_write(&port.base, data, 4, 100) == ESP_LOADER_SUCCESS && uart_writes == 4);
}
