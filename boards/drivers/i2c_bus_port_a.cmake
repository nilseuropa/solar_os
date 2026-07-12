# Port A external I2C bus, shared by any driver/app that talks to a
# device connected there (CardKB, the aqm app's SCD41 CO2 sensor).
# Its own i2c_master instance, separate from the internal bus
# (drivers/i2c_esp_idf.cmake) -- Port A is physically a different I2C
# controller on these boards, not a switched extension of the internal
# one. Boards including this fragment must define
# SOLAR_OS_BOARD_PORT_A_I2C_PORT/PIN_PORT_A_I2C_SDA/SCL.
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/i2c_bus_port_a.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_i2c
    esp_driver_gpio
)
