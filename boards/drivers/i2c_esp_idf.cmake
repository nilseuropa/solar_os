set(SOLAR_OS_BOARD_I2C_DRIVER "esp_idf")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/i2c_bus.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_i2c
)
