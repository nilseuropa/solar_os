set(SOLAR_OS_BOARD_DISPLAY_DRIVER "st7305")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_st7305.c"
    "drivers/rlcd_st7305.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_spi
    u8g2
)
