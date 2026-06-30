set(SOLAR_OS_BOARD_GPIO_DRIVER "esp_idf")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/gpio_port.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
)
