set(SOLAR_OS_BOARD_DISPLAY_DRIVER "ssd1683")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_ssd1683.c"
    "drivers/epd_ssd1683.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_spi
    esp_timer
    u8g2
)
