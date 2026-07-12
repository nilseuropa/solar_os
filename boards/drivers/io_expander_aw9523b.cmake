# AW9523B I2C GPIO expander. Not a tracked SolarOS capability (same
# reasoning as io_expander_ch422g.cmake) -- this only exposes the LCD
# reset line the display driver needs. Boards that include this
# fragment must also include drivers/i2c_esp_idf.cmake (or another I2C
# fragment).
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/io_expander_aw9523b.c"
)
