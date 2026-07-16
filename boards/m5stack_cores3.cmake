set(SOLAR_OS_BOARD_ID "m5stack_cores3")
set(SOLAR_OS_BOARD_NAME "M5Stack CoreS3")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_M5STACK_CORES3")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/cdc_usb_serial_jtag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/pmic_axp2101.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/io_expander_aw9523b.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_ili9341.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_bm8563.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/audio_aw88298.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_bus_port_a.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/touch_ft6336.cmake")

# Minimal port: display (through AXP2101 + AW9523B bring-up), RTC,
# speaker (AW88298, output only), CardKB support on Grove Port A (a
# generic solar_os_cardkb service, not a board-specific driver -- Port
# A's 5V boost is switched on from there via
# io_expander_aw9523b_set_grove_a_boost_enable()), USB CDC + UART,
# connectivity. Not yet wired: SD card (GPIO35 is shared between the
# LCD's DC line and the SD SPI device's MISO line, switched dynamically
# per-operation in M5Stack's own firmware -- solar_os's shared
# sd_card.c/tft_ili9341.c have no notion of a pin doing double duty
# like that, so this needs real driver-level work, not just board
# wiring), ES7210 mic input, BMI270/BMM150 IMU, GC0308 camera, and
# LTR-553 ALS. FT6336U touch is available to apps as a polled driver
# (drivers/touch_ft6336.cmake); there is no system-wide touch input.
set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_SIMD ON)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
set(SOLAR_OS_BOARD_HAS_SPI ON)
set(SOLAR_OS_BOARD_HAS_RTC ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
set(SOLAR_OS_BOARD_HAS_AUDIO ON)
set(SOLAR_OS_BOARD_HAS_CARDKB ON)
set(SOLAR_OS_BOARD_HAS_PORT_A_I2C ON)
set(SOLAR_OS_BOARD_HAS_PM_UART ON)
