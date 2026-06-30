set(SOLAR_OS_BOARD_UART_DRIVER "esp_idf")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/uart_port.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_uart
)
