#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_bus_types.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "waveshare_esp32_s3_rlcd_4_2"
#define SOLAR_OS_BOARD_NAME "Waveshare ESP32-S3-RLCD-4.2"
#define SOLAR_OS_BOARD_VENDOR "Waveshare"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7305"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 400
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 300

#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_40
#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_41
#define SOLAR_OS_BOARD_PIN_LCD_TE GPIO_NUM_6

#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_13
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_14

#define SOLAR_OS_BOARD_PIN_SDMMC_CLK GPIO_NUM_38
#define SOLAR_OS_BOARD_PIN_SDMMC_CMD GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_SDMMC_D0 GPIO_NUM_39

#define SOLAR_OS_BOARD_PIN_BATTERY_ADC GPIO_NUM_4
#define SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO 3.0f

#define SOLAR_OS_BOARD_I2S_PORT I2S_NUM_0
#define SOLAR_OS_BOARD_PIN_I2S_MCLK GPIO_NUM_16
#define SOLAR_OS_BOARD_PIN_I2S_BCLK GPIO_NUM_9
#define SOLAR_OS_BOARD_PIN_I2S_WS GPIO_NUM_45
#define SOLAR_OS_BOARD_PIN_I2S_DIN GPIO_NUM_10
#define SOLAR_OS_BOARD_PIN_I2S_DOUT GPIO_NUM_8
#define SOLAR_OS_BOARD_PIN_AUDIO_PA GPIO_NUM_46
#define SOLAR_OS_BOARD_AUDIO_CODEC_OUT "ES8311"
#define SOLAR_OS_BOARD_AUDIO_CODEC_IN "ES7210"

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_18
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 1
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "2x8 expansion header"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; display facing you"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 2
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 8
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 16
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "GPIO", .position = 1, .row = 0, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
    {.connector = "GPIO", .position = 2, .row = 0, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "GPIO", .position = 3, .row = 0, .column = 2, .pin = GPIO_NUM_19, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO19/D-"}, \
    {.connector = "GPIO", .position = 4, .row = 0, .column = 3, .pin = GPIO_NUM_20, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO20/D+"}, \
    {.connector = "GPIO", .position = 5, .row = 0, .column = 4, .pin = GPIO_NUM_43, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "TXD"}, \
    {.connector = "GPIO", .position = 6, .row = 0, .column = 5, .pin = GPIO_NUM_44, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "RXD"}, \
    {.connector = "GPIO", .position = 7, .row = 0, .column = 6, .pin = GPIO_NUM_13, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "SDA"}, \
    {.connector = "GPIO", .position = 8, .row = 0, .column = 7, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "SCL"}, \
    {.connector = "GPIO", .position = 9, .row = 1, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "GPIO", .position = 10, .row = 1, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "GPIO", .position = 11, .row = 1, .column = 2, .pin = GPIO_NUM_0, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO0"}, \
    {.connector = "GPIO", .position = 12, .row = 1, .column = 3, .pin = GPIO_NUM_1, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO1"}, \
    {.connector = "GPIO", .position = 13, .row = 1, .column = 4, .pin = GPIO_NUM_2, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO2"}, \
    {.connector = "GPIO", .position = 14, .row = 1, .column = 5, .pin = GPIO_NUM_3, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO3"}, \
    {.connector = "GPIO", .position = 15, .row = 1, .column = 6, .pin = GPIO_NUM_17, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO17"}, \
    {.connector = "GPIO", .position = 16, .row = 1, .column = 7, .pin = GPIO_NUM_18, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO18"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_0) | \
                                            (1ULL << GPIO_NUM_1) | \
                                            (1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_13) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_17) | \
                                            (1ULL << GPIO_NUM_18) | \
                                            (1ULL << GPIO_NUM_19) | \
                                            (1ULL << GPIO_NUM_20) | \
                                            (1ULL << GPIO_NUM_43) | \
                                            (1ULL << GPIO_NUM_44))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_1) | \
                                       (1ULL << GPIO_NUM_2) | \
                                       (1ULL << GPIO_NUM_3) | \
                                       (1ULL << GPIO_NUM_17))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "0 1 2 3 13 14 17 18 19 20 43 44"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "1 2 3 17"
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK (1U << SPI3_HOST)
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))
#define SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK (1U << I2S_NUM_1)
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "BOOT/download"}, \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "battery ADC"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display D/C"}, \
    {.pin = 6, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display TE"}, \
    {.pin = 8, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio DOUT"}, \
    {.pin = 9, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio BCLK"}, \
    {.pin = 10, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio DIN"}, \
    {.pin = 11, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display SCK"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display MOSI"}, \
    {.pin = 13, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SDA"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "I2C SCL"}, \
    {.pin = 16, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio MCLK"}, \
    {.pin = 17, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "KEY"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D-/CDC"}, \
    {.pin = 20, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "USB D+/CDC"}, \
    {.pin = 40, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display CS"}, \
    {.pin = 41, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "display reset"}, \
    {.pin = 43, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "UART TX"}, \
    {.pin = 44, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "UART RX"}, \
    {.pin = 45, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio WS"}, \
    {.pin = 46, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "audio PA"}, \
}

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44
#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "spi0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.spi = { \
            .host = SPI2_HOST, \
            .sclk_pin = SOLAR_OS_BOARD_PIN_LCD_SCK, \
            .miso_pin = GPIO_NUM_NC, \
            .mosi_pin = SOLAR_OS_BOARD_PIN_LCD_MOSI, \
            .max_transfer_size = 4092, \
            .cs_count = 1, \
            .cs = {{.name = "display", .pin = SOLAR_OS_BOARD_PIN_LCD_CS}}, \
        }, \
    }, \
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

#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 5
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "st7305", \
        .name = "display0", \
        .binding_count = 4, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .target = "spi0", .value = SOLAR_OS_BOARD_PIN_LCD_CS}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = SOLAR_OS_BOARD_PIN_LCD_DC}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = SOLAR_OS_BOARD_PIN_LCD_RST}, \
        }, \
    }, \
    { \
        .driver = "pcf85063", \
        .name = "rtc0", \
        .binding_count = 2, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x51}, \
        }, \
    }, \
    { \
        .driver = "shtc3", \
        .name = "environment0", \
        .binding_count = 2, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, .value = 0x70}, \
        }, \
    }, \
    { \
        .driver = "battery-adc", \
        .name = "battery0", \
        .binding_count = 2, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_ADC, .role = "battery", .value = SOLAR_OS_BOARD_PIN_BATTERY_ADC}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "divider", .value = 3000}, \
        }, \
    }, \
    { \
        .driver = "es8311-es7210", \
        .name = "audio0", \
        .binding_count = 8, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .target = "i2c0"}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .value = SOLAR_OS_BOARD_I2S_PORT}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "mclk", .value = SOLAR_OS_BOARD_PIN_I2S_MCLK}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .value = SOLAR_OS_BOARD_PIN_I2S_BCLK}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .value = SOLAR_OS_BOARD_PIN_I2S_WS}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .value = SOLAR_OS_BOARD_PIN_I2S_DIN}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dout", .value = SOLAR_OS_BOARD_PIN_I2S_DOUT}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "pa", .value = SOLAR_OS_BOARD_PIN_AUDIO_PA}, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK

#define WS_RLCD_BOARD_NAME SOLAR_OS_BOARD_NAME
#define WS_RLCD_MODULE_NAME SOLAR_OS_BOARD_MODULE_NAME
#define WS_RLCD_DISPLAY_CONTROLLER SOLAR_OS_BOARD_DISPLAY_CONTROLLER
#define WS_RLCD_DISPLAY_WIDTH SOLAR_OS_BOARD_DISPLAY_WIDTH
#define WS_RLCD_DISPLAY_HEIGHT SOLAR_OS_BOARD_DISPLAY_HEIGHT
#define WS_RLCD_PIN_LCD_DC SOLAR_OS_BOARD_PIN_LCD_DC
#define WS_RLCD_PIN_LCD_CS SOLAR_OS_BOARD_PIN_LCD_CS
#define WS_RLCD_PIN_LCD_SCK SOLAR_OS_BOARD_PIN_LCD_SCK
#define WS_RLCD_PIN_LCD_MOSI SOLAR_OS_BOARD_PIN_LCD_MOSI
#define WS_RLCD_PIN_LCD_RST SOLAR_OS_BOARD_PIN_LCD_RST
#define WS_RLCD_PIN_LCD_TE SOLAR_OS_BOARD_PIN_LCD_TE
#define WS_RLCD_PIN_I2C_SDA SOLAR_OS_BOARD_PIN_I2C_SDA
#define WS_RLCD_PIN_I2C_SCL SOLAR_OS_BOARD_PIN_I2C_SCL
#define WS_RLCD_PIN_SDMMC_CLK SOLAR_OS_BOARD_PIN_SDMMC_CLK
#define WS_RLCD_PIN_SDMMC_CMD SOLAR_OS_BOARD_PIN_SDMMC_CMD
#define WS_RLCD_PIN_SDMMC_D0 SOLAR_OS_BOARD_PIN_SDMMC_D0
#define WS_RLCD_PIN_BATTERY_ADC SOLAR_OS_BOARD_PIN_BATTERY_ADC
#define WS_RLCD_BATTERY_ADC_DIVIDER_RATIO SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO
#define WS_RLCD_I2S_PORT SOLAR_OS_BOARD_I2S_PORT
#define WS_RLCD_PIN_I2S_MCLK SOLAR_OS_BOARD_PIN_I2S_MCLK
#define WS_RLCD_PIN_I2S_BCLK SOLAR_OS_BOARD_PIN_I2S_BCLK
#define WS_RLCD_PIN_I2S_WS SOLAR_OS_BOARD_PIN_I2S_WS
#define WS_RLCD_PIN_I2S_DIN SOLAR_OS_BOARD_PIN_I2S_DIN
#define WS_RLCD_PIN_I2S_DOUT SOLAR_OS_BOARD_PIN_I2S_DOUT
#define WS_RLCD_PIN_AUDIO_PA SOLAR_OS_BOARD_PIN_AUDIO_PA
#define WS_RLCD_AUDIO_CODEC_OUT SOLAR_OS_BOARD_AUDIO_CODEC_OUT
#define WS_RLCD_AUDIO_CODEC_IN SOLAR_OS_BOARD_AUDIO_CODEC_IN
#define WS_RLCD_PIN_KEY SOLAR_OS_BOARD_PIN_KEY
#define WS_RLCD_KEY_ACTIVE_LEVEL SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL
#define WS_RLCD_KEY_PULL_UP SOLAR_OS_BOARD_KEY_PULL_UP
#define WS_RLCD_KEY_PULL_DOWN SOLAR_OS_BOARD_KEY_PULL_DOWN
#define WS_RLCD_EXPANSION_GPIO_MASK SOLAR_OS_BOARD_EXPANSION_GPIO_MASK
#define WS_RLCD_USER_GPIO_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define WS_RLCD_UART_PORT SOLAR_OS_BOARD_UART_PORT
#define WS_RLCD_PIN_UART_TX SOLAR_OS_BOARD_PIN_UART_TX
#define WS_RLCD_PIN_UART_RX SOLAR_OS_BOARD_PIN_UART_RX
