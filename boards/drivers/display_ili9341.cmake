set(SOLAR_OS_BOARD_DISPLAY_DRIVER "ili9341")
include("${CMAKE_CURRENT_LIST_DIR}/spi_esp_idf.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_display_ili9341.c"
    "drivers/spi_bus.c"
    "drivers/tft_ili9341.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    u8g2
)
