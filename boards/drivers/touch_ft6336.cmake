# FT6336U capacitive touch (M5Stack Core2 / CoreS3), polled on the
# internal I2C bus at 0x38. There is no system-wide touch input yet;
# apps that want touch poll drivers/touch_ft6336.h themselves.
include("${CMAKE_CURRENT_LIST_DIR}/i2c_esp_idf.cmake")

set(SOLAR_OS_BOARD_HAS_TOUCH ON)
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/touch_ft6336.c"
)
