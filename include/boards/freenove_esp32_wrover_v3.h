#pragma once

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_bus_types.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "freenove_esp32_wrover_v3"
#define SOLAR_OS_BOARD_NAME "Freenove ESP32-WROVER v3.0"
#define SOLAR_OS_BOARD_VENDOR "Freenove"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-WROVER-E-N4R8"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))
#define SOLAR_OS_BOARD_RUNTIME_I2S_PORT_MASK (1U << I2S_NUM_1)

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_0
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 1
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_PIN_SDMMC_CLK GPIO_NUM_14
#define SOLAR_OS_BOARD_PIN_SDMMC_CMD GPIO_NUM_15
#define SOLAR_OS_BOARD_PIN_SDMMC_D0 GPIO_NUM_2

/* The composite backend continuously owns the ESP32 DAC1 output. */
#define SOLAR_OS_BOARD_PIN_COMPOSITE_VIDEO GPIO_NUM_25
#if defined(SOLAR_OS_CVBS_MODE_320X200) && SOLAR_OS_CVBS_MODE_320X200
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "PAL 312p/50 CVBS safe area"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 200
#else
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "PAL 625/50 CVBS"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 384
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 288
#endif
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1
/* PAL scanout expands each line in an ISR and requires the full ESP32 clock. */
#define SOLAR_OS_BOARD_CPU_FLOOR_MHZ 240U

#define SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK ((1U << SPI2_HOST) | (1U << SPI3_HOST))

#define SOLAR_OS_BOARD_BUSES { \
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

#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 2
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "cvbs-pal", \
        .name = "display0", \
        .binding_count = 2, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .value = I2S_NUM_0}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "out", .value = SOLAR_OS_BOARD_PIN_COMPOSITE_VIDEO}, \
        }, \
    }, \
    { \
        .driver = "sdmmc", \
        .name = "storage0", \
        .binding_count = 3, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "clk", .value = SOLAR_OS_BOARD_PIN_SDMMC_CLK}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "cmd", .value = SOLAR_OS_BOARD_PIN_SDMMC_CMD}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "d0", .value = SOLAR_OS_BOARD_PIN_SDMMC_D0}, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "Freenove v3.0 side headers"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; antenna at top, USB connector at bottom"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 20
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 2
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 39
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "L", .position = 1, .row = 0, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "R", .position = 1, .row = 0, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 2, .row = 1, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_CONTROL, .label = "RST"}, \
    {.connector = "R", .position = 2, .row = 1, .column = 1, .pin = GPIO_NUM_23, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO23"}, \
    {.connector = "L", .position = 3, .row = 2, .column = 0, .pin = GPIO_NUM_36, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO36"}, \
    {.connector = "R", .position = 3, .row = 2, .column = 1, .pin = GPIO_NUM_22, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO22"}, \
    {.connector = "L", .position = 4, .row = 3, .column = 0, .pin = GPIO_NUM_39, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO39"}, \
    {.connector = "R", .position = 4, .row = 3, .column = 1, .pin = GPIO_NUM_1, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "TX"}, \
    {.connector = "L", .position = 5, .row = 4, .column = 0, .pin = GPIO_NUM_34, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO34"}, \
    {.connector = "R", .position = 5, .row = 4, .column = 1, .pin = GPIO_NUM_3, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "RX"}, \
    {.connector = "L", .position = 6, .row = 5, .column = 0, .pin = GPIO_NUM_35, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO35"}, \
    {.connector = "R", .position = 6, .row = 5, .column = 1, .pin = GPIO_NUM_21, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO21"}, \
    {.connector = "L", .position = 7, .row = 6, .column = 0, .pin = GPIO_NUM_32, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO32"}, \
    {.connector = "R", .position = 7, .row = 6, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 8, .row = 7, .column = 0, .pin = GPIO_NUM_33, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO33"}, \
    {.connector = "R", .position = 8, .row = 7, .column = 1, .pin = GPIO_NUM_19, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO19"}, \
    {.connector = "L", .position = 9, .row = 8, .column = 0, .pin = GPIO_NUM_25, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO25/PAL"}, \
    {.connector = "R", .position = 9, .row = 8, .column = 1, .pin = GPIO_NUM_18, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO18"}, \
    {.connector = "L", .position = 10, .row = 9, .column = 0, .pin = GPIO_NUM_26, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO26"}, \
    {.connector = "R", .position = 10, .row = 9, .column = 1, .pin = GPIO_NUM_5, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO5"}, \
    {.connector = "L", .position = 11, .row = 10, .column = 0, .pin = GPIO_NUM_27, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO27"}, \
    {.connector = "R", .position = 11, .row = 10, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 12, .row = 11, .column = 0, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO14/SD CLK"}, \
    {.connector = "R", .position = 12, .row = 11, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 13, .row = 12, .column = 0, .pin = GPIO_NUM_12, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO12"}, \
    {.connector = "R", .position = 13, .row = 12, .column = 1, .pin = GPIO_NUM_4, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO4"}, \
    {.connector = "L", .position = 14, .row = 13, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "R", .position = 14, .row = 13, .column = 1, .pin = GPIO_NUM_0, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO0/BOOT/KEY"}, \
    {.connector = "L", .position = 15, .row = 14, .column = 0, .pin = GPIO_NUM_13, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO13"}, \
    {.connector = "R", .position = 15, .row = 14, .column = 1, .pin = GPIO_NUM_2, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO2/SD D0"}, \
    {.connector = "L", .position = 16, .row = 15, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "R", .position = 16, .row = 15, .column = 1, .pin = GPIO_NUM_15, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO15/SD CMD"}, \
    {.connector = "L", .position = 17, .row = 16, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "R", .position = 17, .row = 16, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 18, .row = 17, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "5V"}, \
    {.connector = "R", .position = 18, .row = 17, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "L", .position = 19, .row = 18, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "5V"}, \
    {.connector = "R", .position = 19, .row = 18, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "R", .position = 20, .row = 19, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_0) | \
                                            (1ULL << GPIO_NUM_1) | \
                                            (1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_3) | \
                                            (1ULL << GPIO_NUM_4) | \
                                            (1ULL << GPIO_NUM_5) | \
                                            (1ULL << GPIO_NUM_12) | \
                                            (1ULL << GPIO_NUM_13) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_15) | \
                                            (1ULL << GPIO_NUM_18) | \
                                            (1ULL << GPIO_NUM_19) | \
                                            (1ULL << GPIO_NUM_21) | \
                                            (1ULL << GPIO_NUM_22) | \
                                            (1ULL << GPIO_NUM_23) | \
                                            (1ULL << GPIO_NUM_25) | \
                                            (1ULL << GPIO_NUM_26) | \
                                            (1ULL << GPIO_NUM_27) | \
                                            (1ULL << GPIO_NUM_32) | \
                                            (1ULL << GPIO_NUM_33) | \
                                            (1ULL << GPIO_NUM_34) | \
                                            (1ULL << GPIO_NUM_35) | \
                                            (1ULL << GPIO_NUM_36) | \
                                            (1ULL << GPIO_NUM_39))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_4) | \
                                       (1ULL << GPIO_NUM_5) | \
                                       (1ULL << GPIO_NUM_13) | \
                                       (1ULL << GPIO_NUM_18) | \
                                       (1ULL << GPIO_NUM_19) | \
                                       (1ULL << GPIO_NUM_21) | \
                                       (1ULL << GPIO_NUM_22) | \
                                       (1ULL << GPIO_NUM_23) | \
                                       (1ULL << GPIO_NUM_26) | \
                                       (1ULL << GPIO_NUM_27) | \
                                       (1ULL << GPIO_NUM_32) | \
                                       (1ULL << GPIO_NUM_33) | \
                                       (1ULL << GPIO_NUM_34) | \
                                       (1ULL << GPIO_NUM_35) | \
                                       (1ULL << GPIO_NUM_36) | \
                                       (1ULL << GPIO_NUM_39))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "0 1 2 3 4 5 12 13 14 15 18 19 21 22 23 25 26 27 32 33 34 35 36 39"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "4 5 13 18 19 21 22 23 26 27 32 33 34 35 36 39"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK ((1ULL << GPIO_NUM_32) | \
                                           (1ULL << GPIO_NUM_33) | \
                                           (1ULL << GPIO_NUM_34) | \
                                           (1ULL << GPIO_NUM_35) | \
                                           (1ULL << GPIO_NUM_36) | \
                                           (1ULL << GPIO_NUM_39))
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK ((1ULL << GPIO_NUM_4) | \
                                           (1ULL << GPIO_NUM_5) | \
                                           (1ULL << GPIO_NUM_13) | \
                                           (1ULL << GPIO_NUM_18) | \
                                           (1ULL << GPIO_NUM_19) | \
                                           (1ULL << GPIO_NUM_21) | \
                                           (1ULL << GPIO_NUM_22) | \
                                           (1ULL << GPIO_NUM_23) | \
                                           (1ULL << GPIO_NUM_26) | \
                                           (1ULL << GPIO_NUM_27) | \
                                           (1ULL << GPIO_NUM_32) | \
                                           (1ULL << GPIO_NUM_33))
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 0, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "BOOT/download / KEY"}, \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "CH340 UART TX"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC D0 / onboard LED"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "CH340 UART RX"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion / strapping"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "strapping"}, \
    {.pin = 13, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC CLK"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SDMMC CMD / strapping"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 21, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 22, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 23, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 25, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "PAL composite"}, \
    {.pin = 26, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 27, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 32, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 33, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "expansion"}, \
    {.pin = 34, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
    {.pin = 35, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
    {.pin = 36, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
    {.pin = 39, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
}
