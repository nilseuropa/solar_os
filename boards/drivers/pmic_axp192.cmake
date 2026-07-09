# AXP192 power-management IC bring-up. Not a tracked SolarOS capability
# (there is no board that treats "has a PMIC" as a service-level
# capability yet) -- this only powers the LCD backlight/reset rails
# before the display driver runs. Boards that include this fragment must
# also include drivers/i2c_esp_idf.cmake (or another I2C fragment).
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pmic_axp192.c"
)
