# CH422G I2C GPIO expander. Not a tracked SolarOS capability (there is
# no board that treats "has an IO expander" as a service-level
# capability yet) -- this only exposes the handful of expander-driven
# pins the display/storage drivers need (LCD reset, backlight enable,
# SD card chip-select). Boards that include this fragment must also
# include drivers/i2c_esp_idf.cmake (or another I2C fragment).
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/io_expander_ch422g.c"
)
