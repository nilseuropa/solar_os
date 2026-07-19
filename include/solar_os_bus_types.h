#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLAR_OS_BUS_NAME_MAX 16
#define SOLAR_OS_BUS_OWNER_MAX 24
#define SOLAR_OS_BUS_SPI_CS_MAX 4
#define SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ 100000U
#define SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE 115200U
#define SOLAR_OS_BUS_UART_MIN_BAUD_RATE 300U
#define SOLAR_OS_BUS_UART_MAX_BAUD_RATE 921600U
#define SOLAR_OS_BUS_SPI_DEFAULT_SPEED_HZ 1000000U
#define SOLAR_OS_BUS_SPI_MAX_SPEED_HZ 20000000U

typedef enum {
    SOLAR_OS_BUS_PROTOCOL_I2C,
    SOLAR_OS_BUS_PROTOCOL_SPI,
    SOLAR_OS_BUS_PROTOCOL_UART,
    SOLAR_OS_BUS_PROTOCOL_ONEWIRE,
} solar_os_bus_protocol_t;

typedef enum {
    SOLAR_OS_BUS_ORIGIN_BOARD,
    SOLAR_OS_BUS_ORIGIN_RUNTIME,
} solar_os_bus_origin_t;

typedef enum {
    SOLAR_OS_BUS_SHARED,
    SOLAR_OS_BUS_EXCLUSIVE,
} solar_os_bus_sharing_t;

typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    int pin;
} solar_os_bus_pin_t;

typedef struct {
    int port;
    int sda_pin;
    int scl_pin;
    uint32_t speed_hz;
} solar_os_bus_i2c_config_t;

typedef struct {
    int host;
    int sclk_pin;
    int miso_pin;
    int mosi_pin;
    uint32_t max_transfer_size;
    uint8_t cs_count;
    solar_os_bus_pin_t cs[SOLAR_OS_BUS_SPI_CS_MAX];
} solar_os_bus_spi_config_t;

typedef struct {
    int port;
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate;
} solar_os_bus_uart_config_t;

typedef struct {
    int pin;
} solar_os_bus_onewire_config_t;

typedef union {
    solar_os_bus_i2c_config_t i2c;
    solar_os_bus_spi_config_t spi;
    solar_os_bus_uart_config_t uart;
    solar_os_bus_onewire_config_t onewire;
} solar_os_bus_config_t;

typedef struct {
    const char *name;
    solar_os_bus_protocol_t protocol;
    solar_os_bus_origin_t origin;
    solar_os_bus_sharing_t sharing;
    solar_os_bus_config_t config;
} solar_os_bus_definition_t;

typedef struct {
    bool active;
    bool ready;
    size_t id;
    char name[SOLAR_OS_BUS_NAME_MAX];
    solar_os_bus_protocol_t protocol;
    solar_os_bus_origin_t origin;
    solar_os_bus_sharing_t sharing;
    size_t lease_count;
    solar_os_bus_config_t config;
} solar_os_bus_info_t;

typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    int port;
    int sda_pin;
    int scl_pin;
    uint32_t speed_hz;
} solar_os_i2c_bus_descriptor_t;

typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    int host;
    int sclk_pin;
    int miso_pin;
    int mosi_pin;
    uint32_t max_transfer_size;
    uint8_t cs_count;
    solar_os_bus_pin_t cs[SOLAR_OS_BUS_SPI_CS_MAX];
} solar_os_spi_bus_descriptor_t;

typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    int port;
    int tx_pin;
    int rx_pin;
    uint32_t baud_rate;
} solar_os_uart_bus_descriptor_t;

typedef struct {
    char name[SOLAR_OS_BUS_NAME_MAX];
    int pin;
} solar_os_onewire_bus_descriptor_t;
