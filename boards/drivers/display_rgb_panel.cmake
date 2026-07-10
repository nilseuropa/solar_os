set(SOLAR_OS_BOARD_DISPLAY_DRIVER "rgb_panel")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_rgb_panel.c"
    "drivers/lcd_rgb_panel.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_lcd
    u8g2
)
