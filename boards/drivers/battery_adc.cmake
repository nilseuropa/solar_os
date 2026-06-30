include("${CMAKE_CURRENT_LIST_DIR}/adc_esp_idf.cmake")

set(SOLAR_OS_BOARD_BATTERY_DRIVER "adc")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_battery_adc.c"
    "drivers/battery_adc.c"
)
