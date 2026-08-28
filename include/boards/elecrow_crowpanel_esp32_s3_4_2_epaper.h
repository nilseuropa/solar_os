#pragma once

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_buttons.h"
#include "solar_os_bus_types.h"
#include "solar_os_keys.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "elecrow_crowpanel_esp32_s3_4_2_epaper"
#define SOLAR_OS_BOARD_NAME "Elecrow CrowPanel ESP32-S3 4.2-inch E-paper"
#define SOLAR_OS_BOARD_VENDOR "Elecrow"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N8R8"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))
#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK (1U << SPI3_HOST)
#define SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK (1U << I2S_NUM_1)
#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "spi0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.spi = { \
            .host = SOLAR_OS_BOARD_DISPLAY_SPI_HOST, \
            .sclk_pin = SOLAR_OS_BOARD_PIN_LCD_SCK, \
            .miso_pin = GPIO_NUM_NC, \
            .mosi_pin = SOLAR_OS_BOARD_PIN_LCD_MOSI, \
            .max_transfer_size = 4092, \
            .cs_count = 1, \
            .cs = {{.name = "display", .pin = SOLAR_OS_BOARD_PIN_LCD_CS}}, \
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

#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "SSD1683"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 400
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 300
#define SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION 90
#define SOLAR_OS_BOARD_DISPLAY_SPI_HOST SPI2_HOST

#define SOLAR_OS_BOARD_PIN_LCD_SCK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_LCD_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_47
#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_46
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_45
#define SOLAR_OS_BOARD_PIN_LCD_BUSY GPIO_NUM_48
#define SOLAR_OS_BOARD_PIN_LCD_POWER GPIO_NUM_7

#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 1
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "ssd1683", \
        .name = "display0", \
        .binding_count = 8, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .target = "spi0", .value = SOLAR_OS_BOARD_PIN_LCD_CS}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = SOLAR_OS_BOARD_PIN_LCD_DC}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = SOLAR_OS_BOARD_PIN_LCD_RST}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .value = SOLAR_OS_BOARD_PIN_LCD_BUSY}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power", .value = SOLAR_OS_BOARD_PIN_LCD_POWER}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "clock", .value = 10000}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 0}, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_SPI_HOST SPI3_HOST
#define SOLAR_OS_BOARD_SPI_NAME "SD-SPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_39
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_13
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_40
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_SPI_RESOURCE_OWNER "board:sdspi"
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_10
#define SOLAR_OS_BOARD_PIN_SD_POWER GPIO_NUM_42
#define SOLAR_OS_BOARD_SD_POWER_ACTIVE_LEVEL 1

#define SOLAR_OS_BOARD_PIN_STATUS_LED GPIO_NUM_41
#define SOLAR_OS_BOARD_STATUS_LED_ACTIVE_LEVEL 1

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_2
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 0
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_BUTTONS { \
    {.pin = GPIO_NUM_1, .name = "EXIT", .key = SOLAR_OS_KEY_APP_EXIT, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
    {.pin = GPIO_NUM_6, .name = "ROTARY-DOWN", .key = SOLAR_OS_KEY_DOWN, .active_low = true, .emit_on_release = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
    {.pin = GPIO_NUM_4, .name = "ROTARY-UP", .key = SOLAR_OS_KEY_UP, .active_low = true, .emit_on_release = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
    {.pin = GPIO_NUM_5, .name = "ROTARY-OK", .key = SOLAR_OS_KEY_ENTER, .active_low = true, .emit_on_release = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
}

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "U5 2x10 GPIO header"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; display facing you"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 2
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 10
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 20
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "U5", .position = 1, .row = 0, .column = 0, .pin = GPIO_NUM_3, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO3"}, \
    {.connector = "U5", .position = 2, .row = 0, .column = 1, .pin = GPIO_NUM_9, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO9"}, \
    {.connector = "U5", .position = 3, .row = 0, .column = 2, .pin = GPIO_NUM_15, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO15"}, \
    {.connector = "U5", .position = 4, .row = 0, .column = 3, .pin = GPIO_NUM_17, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO17"}, \
    {.connector = "U5", .position = 5, .row = 0, .column = 4, .pin = GPIO_NUM_19, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO19"}, \
    {.connector = "U5", .position = 6, .row = 0, .column = 5, .pin = GPIO_NUM_21, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO21"}, \
    {.connector = "U5", .position = 7, .row = 0, .column = 6, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "U5", .position = 8, .row = 0, .column = 7, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "U5", .position = 9, .row = 0, .column = 8, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "U5", .position = 10, .row = 0, .column = 9, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "U5", .position = 20, .row = 1, .column = 0, .pin = GPIO_NUM_8, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO8"}, \
    {.connector = "U5", .position = 19, .row = 1, .column = 1, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO14"}, \
    {.connector = "U5", .position = 18, .row = 1, .column = 2, .pin = GPIO_NUM_16, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO16"}, \
    {.connector = "U5", .position = 17, .row = 1, .column = 3, .pin = GPIO_NUM_18, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO18"}, \
    {.connector = "U5", .position = 16, .row = 1, .column = 4, .pin = GPIO_NUM_20, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO20"}, \
    {.connector = "U5", .position = 15, .row = 1, .column = 5, .pin = GPIO_NUM_38, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO38"}, \
    {.connector = "U5", .position = 14, .row = 1, .column = 6, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
    {.connector = "U5", .position = 13, .row = 1, .column = 7, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
    {.connector = "U5", .position = 12, .row = 1, .column = 8, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
    {.connector = "U5", .position = 11, .row = 1, .column = 9, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_8) | \
                                            (1ULL << GPIO_NUM_9) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_15) | \
                                            (1ULL << GPIO_NUM_16) | \
                                            (1ULL << GPIO_NUM_17) | \
                                            (1ULL << GPIO_NUM_18) | \
                                            (1ULL << GPIO_NUM_19) | \
                                            (1ULL << GPIO_NUM_20) | \
                                            (1ULL << GPIO_NUM_21) | \
                                            (1ULL << GPIO_NUM_38))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_8) | \
                                       (1ULL << GPIO_NUM_9) | \
                                       (1ULL << GPIO_NUM_14) | \
                                       (1ULL << GPIO_NUM_15) | \
                                       (1ULL << GPIO_NUM_16) | \
                                       (1ULL << GPIO_NUM_17) | \
                                       (1ULL << GPIO_NUM_18) | \
                                       (1ULL << GPIO_NUM_19) | \
                                       (1ULL << GPIO_NUM_20) | \
                                       (1ULL << GPIO_NUM_21) | \
                                       (1ULL << GPIO_NUM_38))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "3 8 9 14 15 16 17 18 19 20 21 38"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "8 9 14 15 16 17 18 19 20 21 38"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_8) | \
                                           (1ULL << GPIO_NUM_9) | \
                                           (1ULL << GPIO_NUM_14) | \
                                           (1ULL << GPIO_NUM_15) | \
                                           (1ULL << GPIO_NUM_16) | \
                                           (1ULL << GPIO_NUM_17) | \
                                           (1ULL << GPIO_NUM_18) | \
                                           (1ULL << GPIO_NUM_19) | \
                                           (1ULL << GPIO_NUM_20))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "BOOT/download"}, \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EXIT button"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "MENU / SolarOS KEY"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "expansion / strapping"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "rotary up"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "rotary confirm"}, \
    {.pin = 6, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "rotary down"}, \
    {.pin = 7, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD power"}, \
    {.pin = 8, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 9, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 10, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD chip select"}, \
    {.pin = 11, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD MOSI"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD SCK"}, \
    {.pin = 13, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD MISO"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 16, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 17, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / USB D- capable"}, \
    {.pin = 20, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / USB D+ capable"}, \
    {.pin = 21, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 38, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 39, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD SCK"}, \
    {.pin = 40, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD MOSI"}, \
    {.pin = 41, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "status LED"}, \
    {.pin = 42, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD power"}, \
    {.pin = 43, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "UART TX / USB bridge"}, \
    {.pin = 44, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "UART RX / USB bridge"}, \
    {.pin = 45, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD chip select / strapping"}, \
    {.pin = 46, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD D/C / strapping"}, \
    {.pin = 47, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD reset"}, \
    {.pin = 48, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "EPD busy"}, \
}
