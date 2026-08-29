include("${CMAKE_CURRENT_LIST_DIR}/adc_esp_idf.cmake")

set(SOLAR_OS_BOARD_BATTERY_DRIVER "adc")
list(APPEND SOLAR_OS_BOARD_REQUIRED_PACKAGES driver_battery_adc)
