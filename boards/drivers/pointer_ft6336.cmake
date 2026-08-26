include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")

set(SOLAR_OS_BOARD_POINTER_DRIVER "ft6336")
set(SOLAR_OS_BOARD_POINTER_NEEDS_I2C ON)
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_pointer_ft6336.c"
    "drivers/ft6336.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
)
