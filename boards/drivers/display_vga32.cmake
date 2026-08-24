set(SOLAR_OS_BOARD_DISPLAY_DRIVER "vga32")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_vga32.c"
    "drivers/vga32.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_hw_support
    esp_rom
    esp_system
    hal
    u8g2
)
