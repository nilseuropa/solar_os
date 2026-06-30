include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")

set(SOLAR_OS_BOARD_SENSOR_DRIVER "shtc3")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_sensors_shtc3.c"
    "drivers/shtc3.c"
)
