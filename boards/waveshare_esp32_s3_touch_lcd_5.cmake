set(SOLAR_OS_BOARD_ID "waveshare_esp32_s3_touch_lcd_5")
set(SOLAR_OS_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-5")
set(SOLAR_OS_BOARD_DEFINE "SOLAR_OS_BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5")

include("${CMAKE_CURRENT_LIST_DIR}/drivers/cdc_usb_serial_jtag.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/uart_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/i2c_esp_idf.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/io_expander_ch422g.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/display_rgb_panel.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/storage_sdspi.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/drivers/rtc_pcf85063.cmake")

# Minimal port: RGB display (through CH422G expander bring-up), RTC,
# SD, UART, connectivity. No touch (GT911 -- SolarOS has no
# touch-input capability yet) or RS485. USB native CDC and UART are
# both wired as console/shell candidates; UART lives on GPIO15/16,
# the board's stock CAN pins (TJA1051 TXD/RXD) -- this only becomes a
# real point-to-point serial port once that transceiver is swapped for
# a MAX232 on the hardware side, which drops CAN support in exchange.
set(SOLAR_OS_BOARD_HAS_PSRAM ON)
set(SOLAR_OS_BOARD_PSRAM_BYTES 8388608)
set(SOLAR_OS_BOARD_HAS_SIMD ON)
set(SOLAR_OS_BOARD_HAS_DISPLAY ON)
set(SOLAR_OS_BOARD_HAS_GFX ON)
set(SOLAR_OS_BOARD_HAS_CDC ON)
set(SOLAR_OS_BOARD_HAS_UART ON)
set(SOLAR_OS_BOARD_HAS_SD ON)
set(SOLAR_OS_BOARD_HAS_I2C ON)
set(SOLAR_OS_BOARD_HAS_SPI ON)
set(SOLAR_OS_BOARD_HAS_RTC ON)
set(SOLAR_OS_BOARD_HAS_WIFI ON)
set(SOLAR_OS_BOARD_HAS_BLE ON)
