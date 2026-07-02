#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_expansion_types.h"

#define SOLAR_OS_BOARD_ID "esp32_s3_devkitc1_n16r8"
#define SOLAR_OS_BOARD_NAME "Espressif ESP32-S3-DevKitC-1-N16R8"
#define SOLAR_OS_BOARD_VENDOR "Espressif"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44

#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_8
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_9
#define SOLAR_OS_BOARD_EXPANSION_I2C_BUSES { \
    {.name = "i2c0", .port = SOLAR_OS_BOARD_I2C_PORT, .sda_pin = SOLAR_OS_BOARD_PIN_I2C_SDA, .scl_pin = SOLAR_OS_BOARD_PIN_I2C_SCL}, \
}

#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "FSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_13
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_SPI_CS_SLOTS { \
    {.pin = GPIO_NUM_10, .name = "gpio10"}, \
    {.pin = GPIO_NUM_5, .name = "gpio5"}, \
    {.pin = GPIO_NUM_6, .name = "gpio6"}, \
    {.pin = GPIO_NUM_7, .name = "gpio7"}, \
}
#define SOLAR_OS_BOARD_EXPANSION_SPI_BUSES { \
    { \
        .name = "spi0", \
        .host = SOLAR_OS_BOARD_SPI_HOST, \
        .sclk_pin = SOLAR_OS_BOARD_PIN_SPI_SCLK, \
        .miso_pin = SOLAR_OS_BOARD_PIN_SPI_MISO, \
        .mosi_pin = SOLAR_OS_BOARD_PIN_SPI_MOSI, \
        .max_transfer_size = SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ, \
        .cs_count = 4, \
        .cs = { \
            {.name = "gpio10", .pin = GPIO_NUM_10}, \
            {.name = "gpio5", .pin = GPIO_NUM_5}, \
            {.name = "gpio6", .pin = GPIO_NUM_6}, \
            {.name = "gpio7", .pin = GPIO_NUM_7}, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_0) | \
                                            (1ULL << GPIO_NUM_1) | \
                                            (1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_4) | \
                                            (1ULL << GPIO_NUM_5) | \
                                            (1ULL << GPIO_NUM_6) | \
                                            (1ULL << GPIO_NUM_7) | \
                                            (1ULL << GPIO_NUM_8) | \
                                            (1ULL << GPIO_NUM_9) | \
                                            (1ULL << GPIO_NUM_10) | \
                                            (1ULL << GPIO_NUM_11) | \
                                            (1ULL << GPIO_NUM_12) | \
                                            (1ULL << GPIO_NUM_13) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_15) | \
                                            (1ULL << GPIO_NUM_16) | \
                                            (1ULL << GPIO_NUM_17) | \
                                            (1ULL << GPIO_NUM_18) | \
                                            (1ULL << GPIO_NUM_19) | \
                                            (1ULL << GPIO_NUM_20) | \
                                            (1ULL << GPIO_NUM_21) | \
                                            (1ULL << GPIO_NUM_35) | \
                                            (1ULL << GPIO_NUM_36) | \
                                            (1ULL << GPIO_NUM_37) | \
                                            (1ULL << GPIO_NUM_38) | \
                                            (1ULL << GPIO_NUM_39) | \
                                            (1ULL << GPIO_NUM_40) | \
                                            (1ULL << GPIO_NUM_41) | \
                                            (1ULL << GPIO_NUM_42) | \
                                            (1ULL << GPIO_NUM_43) | \
                                            (1ULL << GPIO_NUM_44) | \
                                            (1ULL << GPIO_NUM_45) | \
                                            (1ULL << GPIO_NUM_46) | \
                                            (1ULL << GPIO_NUM_47) | \
                                            (1ULL << GPIO_NUM_48))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_1) | \
                                       (1ULL << GPIO_NUM_2) | \
                                       (1ULL << GPIO_NUM_4) | \
                                       (1ULL << GPIO_NUM_5) | \
                                       (1ULL << GPIO_NUM_6) | \
                                       (1ULL << GPIO_NUM_7) | \
                                       (1ULL << GPIO_NUM_10) | \
                                       (1ULL << GPIO_NUM_14) | \
                                       (1ULL << GPIO_NUM_15) | \
                                       (1ULL << GPIO_NUM_16) | \
                                       (1ULL << GPIO_NUM_17) | \
                                       (1ULL << GPIO_NUM_18) | \
                                       (1ULL << GPIO_NUM_21) | \
                                       (1ULL << GPIO_NUM_39) | \
                                       (1ULL << GPIO_NUM_40) | \
                                       (1ULL << GPIO_NUM_41) | \
                                       (1ULL << GPIO_NUM_42) | \
                                       (1ULL << GPIO_NUM_47))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 35 36 37 38 39 40 41 42 43 44 45 46 47 48"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "1 2 4 5 6 7 10 14 15 16 17 18 21 39 40 41 42 47"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_1) | \
                                           (1ULL << GPIO_NUM_2) | \
                                           (1ULL << GPIO_NUM_4) | \
                                           (1ULL << GPIO_NUM_5) | \
                                           (1ULL << GPIO_NUM_6) | \
                                           (1ULL << GPIO_NUM_7) | \
                                           (1ULL << GPIO_NUM_10) | \
                                           (1ULL << GPIO_NUM_14) | \
                                           (1ULL << GPIO_NUM_15) | \
                                           (1ULL << GPIO_NUM_16) | \
                                           (1ULL << GPIO_NUM_17) | \
                                           (1ULL << GPIO_NUM_18))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .runtime_allowed = false, .role = "BOOT/download"}, \
    {.pin = 1, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 2, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 3, .runtime_allowed = false, .role = "strapping"}, \
    {.pin = 4, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 5, .runtime_allowed = true, .role = "expansion / SPI CS"}, \
    {.pin = 6, .runtime_allowed = true, .role = "expansion / SPI CS"}, \
    {.pin = 7, .runtime_allowed = true, .role = "expansion / SPI CS"}, \
    {.pin = 8, .runtime_allowed = false, .role = "I2C SDA"}, \
    {.pin = 9, .runtime_allowed = false, .role = "I2C SCL"}, \
    {.pin = 10, .runtime_allowed = true, .role = "expansion / SPI CS"}, \
    {.pin = 11, .runtime_allowed = false, .role = "SPI MOSI"}, \
    {.pin = 12, .runtime_allowed = false, .role = "SPI SCK"}, \
    {.pin = 13, .runtime_allowed = false, .role = "SPI MISO"}, \
    {.pin = 14, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 15, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 16, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 17, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 18, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 19, .runtime_allowed = false, .role = "USB D-/CDC"}, \
    {.pin = 20, .runtime_allowed = false, .role = "USB D+/CDC"}, \
    {.pin = 21, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 35, .runtime_allowed = false, .role = "Octal PSRAM"}, \
    {.pin = 36, .runtime_allowed = false, .role = "Octal PSRAM"}, \
    {.pin = 37, .runtime_allowed = false, .role = "Octal PSRAM"}, \
    {.pin = 38, .runtime_allowed = false, .role = "RGB LED (v1.1)"}, \
    {.pin = 39, .runtime_allowed = true, .role = "expansion / JTAG MTCK"}, \
    {.pin = 40, .runtime_allowed = true, .role = "expansion / JTAG MTDO"}, \
    {.pin = 41, .runtime_allowed = true, .role = "expansion / JTAG MTDI"}, \
    {.pin = 42, .runtime_allowed = true, .role = "expansion / JTAG MTMS"}, \
    {.pin = 43, .runtime_allowed = false, .role = "UART TX"}, \
    {.pin = 44, .runtime_allowed = false, .role = "UART RX"}, \
    {.pin = 45, .runtime_allowed = false, .role = "strapping"}, \
    {.pin = 46, .runtime_allowed = false, .role = "strapping"}, \
    {.pin = 47, .runtime_allowed = true, .role = "expansion"}, \
    {.pin = 48, .runtime_allowed = false, .role = "RGB LED (v1.0)"}, \
}
