set(SOLAR_OS_BOARD_ID "m5stack_core2")
set(SOLAR_OS_BOARD_NAME "M5Stack Core2")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_M5STACK_CORE2")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/pmic_axp192.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_ili9341.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdspi.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_bm8563.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_bus_port_a.cmake")

# Minimal port: display, storage, connectivity, RTC. No buttons/dpad/
# battery/audio/sensors/status LED/runtime GPIO yet -- Core2 exposes
# those through the AXP192 PMIC and a capacitive touch controller
# instead of the discrete GPIOs odroid_go uses, which needs its own
# driver work.
set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
set(SOLAR_OS_BOARD_HAS_SPI ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
set(SOLAR_OS_BOARD_HAS_RTC ON)
set(SOLAR_OS_BOARD_HAS_CARDKB ON)
set(SOLAR_OS_BOARD_HAS_PM_UART ON)
set(SOLAR_OS_BOARD_HAS_PORT_A_I2C ON)
