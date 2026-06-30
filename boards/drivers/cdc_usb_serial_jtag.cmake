set(SOLAR_OS_BOARD_CDC_DRIVER "usb_serial_jtag")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/cdc_port.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_usb_serial_jtag
)
