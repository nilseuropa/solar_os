include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")

set(SOLAR_OS_BOARD_RTC_DRIVER "pcf85063")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_rtc_pcf85063.c"
    "drivers/rtc_pcf85063.c"
)
