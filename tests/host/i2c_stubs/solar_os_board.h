#pragma once

#define SOLAR_OS_BOARD_HAS_I2C 1
#define SOLAR_OS_BOARD_HAS_EXPANSION_I2C 1
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK 0U
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK 0U
#define SOLAR_OS_BOARD_I2C_PORT 0
#define SOLAR_OS_BOARD_PIN_I2C_SDA 8
#define SOLAR_OS_BOARD_PIN_I2C_SCL 9
#define SOLAR_OS_BOARD_BUSES { \
    { .name = "i2c0", .protocol = SOLAR_OS_BUS_PROTOCOL_I2C, \
      .origin = SOLAR_OS_BUS_ORIGIN_BOARD, .sharing = SOLAR_OS_BUS_SHARED, \
      .config.i2c = { .port = 0, .sda_pin = 8, .scl_pin = 9, .speed_hz = 100000 } }, \
    { .name = "i2c1", .protocol = SOLAR_OS_BUS_PROTOCOL_I2C, \
      .origin = SOLAR_OS_BUS_ORIGIN_BOARD, .sharing = SOLAR_OS_BUS_SHARED, \
      .config.i2c = { .port = 1, .sda_pin = 10, .scl_pin = 11, .speed_hz = 100000 } }, \
}
