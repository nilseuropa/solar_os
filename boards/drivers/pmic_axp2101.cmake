# AXP2101 power-management IC bring-up. Not a tracked SolarOS capability
# (same reasoning as pmic_axp192.cmake) -- this only powers the rails
# the display driver needs before it runs. Boards that include this
# fragment must also include drivers/i2c_esp_idf.cmake (or another I2C
# fragment).
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pmic_axp2101.c"
)
