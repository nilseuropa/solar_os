#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_bus_types.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "freenove_esp32_s3_display_4_0"
#define SOLAR_OS_BOARD_NAME "Freenove ESP32-S3 Display 4.0-inch (FNK0104S)"
#define SOLAR_OS_BOARD_VENDOR "Freenove"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7796"
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER_ST7796 1
#define SOLAR_OS_BOARD_DISPLAY_DRIVER_NAME "st7796"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 480
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 320
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 480
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0x48
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_10
#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_46
#define SOLAR_OS_BOARD_PIN_LCD_BL GPIO_NUM_45
#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_LCD_TE GPIO_NUM_NC
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL 1
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM 1
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ 20000U

#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "FSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK (1U << SPI3_HOST)

#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_16
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_15
#define SOLAR_OS_BOARD_PIN_TOUCH_INT GPIO_NUM_17
#define SOLAR_OS_BOARD_PIN_TOUCH_RST GPIO_NUM_18

#define SOLAR_OS_BOARD_PIN_SDMMC_CLK GPIO_NUM_38
#define SOLAR_OS_BOARD_PIN_SDMMC_CMD GPIO_NUM_40
#define SOLAR_OS_BOARD_PIN_SDMMC_D0 GPIO_NUM_39
#define SOLAR_OS_BOARD_PIN_SDMMC_D1 GPIO_NUM_41
#define SOLAR_OS_BOARD_PIN_SDMMC_D2 GPIO_NUM_48
#define SOLAR_OS_BOARD_PIN_SDMMC_D3 GPIO_NUM_47

#define SOLAR_OS_BOARD_PIN_BATTERY_ADC GPIO_NUM_9
#define SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO 2.0f

#define SOLAR_OS_BOARD_I2S_PORT I2S_NUM_0
#define SOLAR_OS_BOARD_PIN_I2S_MCLK GPIO_NUM_4
#define SOLAR_OS_BOARD_PIN_I2S_BCLK GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_I2S_DIN GPIO_NUM_6
#define SOLAR_OS_BOARD_PIN_I2S_WS GPIO_NUM_7
#define SOLAR_OS_BOARD_PIN_I2S_DOUT GPIO_NUM_8
#define SOLAR_OS_BOARD_PIN_AUDIO_PA GPIO_NUM_1
#define SOLAR_OS_BOARD_AUDIO_CODEC_OUT "ES8311"
#define SOLAR_OS_BOARD_AUDIO_CODEC_IN "ES8311"
#define SOLAR_OS_BOARD_AUDIO_ES8311_DUPLEX 1

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_0
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 1
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))
#define SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK (1U << I2S_NUM_1)

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "UART / I2C / GPIO connectors"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW "component side; connector labels as printed"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 6
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 3
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 14
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "UART", .position = 1, .row = 0, .column = 0, .pin = GPIO_NUM_43, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "TXD"}, \
    {.connector = "UART", .position = 2, .row = 1, .column = 0, .pin = GPIO_NUM_44, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "RXD"}, \
    {.connector = "UART", .position = 3, .row = 2, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "5V"}, \
    {.connector = "UART", .position = 4, .row = 3, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "I2C", .position = 1, .row = 0, .column = 1, .pin = GPIO_NUM_16, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO16/SDA"}, \
    {.connector = "I2C", .position = 2, .row = 1, .column = 1, .pin = GPIO_NUM_15, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO15/SCL"}, \
    {.connector = "I2C", .position = 3, .row = 2, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "I2C", .position = 4, .row = 3, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "GPIO", .position = 1, .row = 0, .column = 2, .pin = GPIO_NUM_2, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO2"}, \
    {.connector = "GPIO", .position = 2, .row = 1, .column = 2, .pin = GPIO_NUM_3, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO3"}, \
    {.connector = "GPIO", .position = 3, .row = 2, .column = 2, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO14"}, \
    {.connector = "GPIO", .position = 4, .row = 3, .column = 2, .pin = GPIO_NUM_21, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO21"}, \
    {.connector = "GPIO", .position = 5, .row = 4, .column = 2, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "GPIO", .position = 6, .row = 5, .column = 2, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_15) | \
                                            (1ULL << GPIO_NUM_16) | \
                                            (1ULL << GPIO_NUM_21) | \
                                            (1ULL << GPIO_NUM_43) | \
                                            (1ULL << GPIO_NUM_44))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_2) | \
                                       (1ULL << GPIO_NUM_3) | \
                                       (1ULL << GPIO_NUM_14) | \
                                       (1ULL << GPIO_NUM_21))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "2 3 14 15 16 21 43 44"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "2 3 14 21"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_2) | \
                                           (1ULL << GPIO_NUM_3) | \
                                           (1ULL << GPIO_NUM_14))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK

#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "BOOT/download / KEY"}, \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "speaker amplifier enable"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "GPIO connector"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "GPIO connector"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio MCLK"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio BCLK"}, \
    {.pin = 6, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio DIN"}, \
    {.pin = 7, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio WS"}, \
    {.pin = 8, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio DOUT"}, \
    {.pin = 9, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "battery ADC"}, \
    {.pin = 10, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD CS"}, \
    {.pin = 11, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD MOSI"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD SCLK"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "GPIO connector"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SCL"}, \
    {.pin = 16, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SDA"}, \
    {.pin = 17, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "touch interrupt"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "touch reset"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D-"}, \
    {.pin = 20, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D+"}, \
    {.pin = 21, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "GPIO connector"}, \
    {.pin = 38, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC CLK"}, \
    {.pin = 39, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC D0"}, \
    {.pin = 40, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC CMD"}, \
    {.pin = 41, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC D1"}, \
    {.pin = 42, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "WS2812"}, \
    {.pin = 43, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "UART TX"}, \
    {.pin = 44, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "UART RX"}, \
    {.pin = 45, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD backlight"}, \
    {.pin = 46, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD DC / strapping"}, \
    {.pin = 47, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC D3"}, \
    {.pin = 48, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC D2"}, \
}

#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "i2c0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_I2C, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.i2c = { \
            .port = SOLAR_OS_BOARD_I2C_PORT, \
            .sda_pin = SOLAR_OS_BOARD_PIN_I2C_SDA, \
            .scl_pin = SOLAR_OS_BOARD_PIN_I2C_SCL, \
            .speed_hz = SOLAR_OS_BUS_I2C_DEFAULT_SPEED_HZ, \
        }, \
    }, \
    { \
        .name = "uart0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_UART, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_EXCLUSIVE, \
        .config.uart = { \
            .port = SOLAR_OS_BOARD_UART_PORT, \
            .tx_pin = SOLAR_OS_BOARD_PIN_UART_TX, \
            .rx_pin = SOLAR_OS_BOARD_PIN_UART_RX, \
            .baud_rate = SOLAR_OS_BUS_UART_DEFAULT_BAUD_RATE, \
        }, \
    }, \
}
