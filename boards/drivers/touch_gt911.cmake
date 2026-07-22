# GT911 capacitive touch (Waveshare ESP32-S3-Touch-LCD-5), polled on
# the internal I2C bus at 0x5D/0x14 (address latched at reset; both
# probed). Reset is on the CH422G expander. There is no system-wide
# touch input; apps poll the solar_os_touch.h facade.
include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_TOUCH ON)
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/touch_gt911.c"
)
