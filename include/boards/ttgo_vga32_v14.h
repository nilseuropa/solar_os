#pragma once

#include "driver/gpio.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_bus_types.h"
#include "solar_os_pin_types.h"

#define SOLAR_OS_BOARD_ID "ttgo_vga32_v14"
#define SOLAR_OS_BOARD_NAME "LilyGO TTGO VGA32 v1.4"
#define SOLAR_OS_BOARD_VENDOR "LilyGO"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-PICO-D4 + 8MB PSRAM"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3

#if defined(SOLAR_OS_VGA_MODE_320X200) && SOLAR_OS_VGA_MODE_320X200
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "VGA 320x200@70Hz"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 200
#elif defined(SOLAR_OS_VGA_MODE_320X240) && SOLAR_OS_VGA_MODE_320X240
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "VGA 320x240@60Hz"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 240
#elif defined(SOLAR_OS_VGA_MODE_640X400) && SOLAR_OS_VGA_MODE_640X400
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "VGA 640x400@70Hz"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 640
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 400
#else
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "VGA 640x480@60Hz"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 640
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 480
#endif
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1
#define SOLAR_OS_BOARD_CPU_FLOOR_MHZ 240U
#define SOLAR_OS_BOARD_DISPLAY_FRAME_INTERVAL_MS 33U

/* TTGO VGA32 v1.4 uses FabGL's standard 2-bit-per-channel VGA wiring. */
#define SOLAR_OS_BOARD_PIN_VGA_RED0 GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_VGA_RED1 GPIO_NUM_22
#define SOLAR_OS_BOARD_PIN_VGA_GREEN0 GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_VGA_GREEN1 GPIO_NUM_19
#define SOLAR_OS_BOARD_PIN_VGA_BLUE0 GPIO_NUM_4
#define SOLAR_OS_BOARD_PIN_VGA_BLUE1 GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_VGA_HSYNC GPIO_NUM_23
#define SOLAR_OS_BOARD_PIN_VGA_VSYNC GPIO_NUM_15

/* VGA scanout continuously owns I2S1 in LCD/parallel mode. */
#define SOLAR_OS_BOARD_VGA_I2S_PORT I2S_NUM_1

/* GPIO25 feeds the board's mono line output and NS4150 speaker amplifier. */
#define SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS GPIO_NUM_25
#define SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG GPIO_NUM_NC
#define SOLAR_OS_BOARD_AUDIO_CODEC_OUT "ESP32-DAC"
#define SOLAR_OS_BOARD_AUDIO_CODEC_IN "-"

/* TTGO VGA32 v1.4 SD differs from v1.2 on MOSI and MISO. */
#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "HSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_14
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_2
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_12
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_13
#define SOLAR_OS_BOARD_SPI_CS_SLOTS { \
    {.pin = SOLAR_OS_BOARD_PIN_SD_CARD_CS, .name = "sd"}, \
}

#define SOLAR_OS_BOARD_PIN_PS2_KEYBOARD_DATA GPIO_NUM_32
#define SOLAR_OS_BOARD_PIN_PS2_KEYBOARD_CLOCK GPIO_NUM_33
#define SOLAR_OS_BOARD_PIN_PS2_MOUSE_DATA GPIO_NUM_27
#define SOLAR_OS_BOARD_PIN_PS2_MOUSE_CLOCK GPIO_NUM_26
#define SOLAR_OS_BOARD_AUTOSTART_PS2_BUS "ps2kbd0"
#define SOLAR_OS_BOARD_DEFAULT_BLE_ENABLED 0

#define SOLAR_OS_BOARD_BUSES { \
    { \
        .name = "spi0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_SPI, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_SHARED, \
        .config.spi = { \
            .host = SOLAR_OS_BOARD_SPI_HOST, \
            .sclk_pin = SOLAR_OS_BOARD_PIN_SPI_SCLK, \
            .miso_pin = SOLAR_OS_BOARD_PIN_SPI_MISO, \
            .mosi_pin = SOLAR_OS_BOARD_PIN_SPI_MOSI, \
            .max_transfer_size = SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ, \
            .cs_count = 1, \
            .cs = { \
                {.name = "sd", .pin = SOLAR_OS_BOARD_PIN_SD_CARD_CS}, \
            }, \
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
    { \
        .name = "ps2kbd0", \
        .protocol = SOLAR_OS_BUS_PROTOCOL_PS2, \
        .origin = SOLAR_OS_BUS_ORIGIN_BOARD, \
        .sharing = SOLAR_OS_BUS_EXCLUSIVE, \
        .config.ps2 = { \
            .clock_pin = SOLAR_OS_BOARD_PIN_PS2_KEYBOARD_CLOCK, \
            .data_pin = SOLAR_OS_BOARD_PIN_PS2_KEYBOARD_DATA, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "GPIO expansion header"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; VGA connector at top"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 2
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 4
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 8
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "GPIO", .position = 1, .row = 0, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "GPIO", .position = 2, .row = 0, .column = 1, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "GPIO", .position = 3, .row = 0, .column = 2, .pin = GPIO_NUM_39, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO39"}, \
    {.connector = "GPIO", .position = 4, .row = 0, .column = 3, .pin = GPIO_NUM_34, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO34"}, \
    {.connector = "GPIO", .position = 5, .row = 1, .column = 0, .pin = GPIO_NUM_12, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO12/SD MOSI"}, \
    {.connector = "GPIO", .position = 6, .row = 1, .column = 1, .pin = GPIO_NUM_14, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO14/SD CLK"}, \
    {.connector = "GPIO", .position = 7, .row = 1, .column = 2, .pin = GPIO_NUM_13, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO13/SD CS"}, \
    {.connector = "GPIO", .position = 8, .row = 1, .column = 3, .pin = GPIO_NUM_2, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO2/SD MISO"}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_2) | \
                                            (1ULL << GPIO_NUM_12) | \
                                            (1ULL << GPIO_NUM_13) | \
                                            (1ULL << GPIO_NUM_14) | \
                                            (1ULL << GPIO_NUM_34) | \
                                            (1ULL << GPIO_NUM_39))
#define SOLAR_OS_BOARD_USER_GPIO_MASK ((1ULL << GPIO_NUM_34) | \
                                       (1ULL << GPIO_NUM_39))
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "2 12 13 14 34 39"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "34 39"
#define SOLAR_OS_BOARD_EXPANSION_ADC_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 1, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "USB-UART TX"}, \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD MISO / strapping"}, \
    {.pin = 3, .policy = SOLAR_OS_PIN_POLICY_RELEASABLE, .role = "USB-UART RX"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA blue0"}, \
    {.pin = 5, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA blue1 / strapping"}, \
    {.pin = 12, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD MOSI / strapping"}, \
    {.pin = 13, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD CS"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "SD clock"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA VSync / strapping"}, \
    {.pin = 18, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA green0"}, \
    {.pin = 19, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA green1"}, \
    {.pin = 21, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA red0"}, \
    {.pin = 22, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA red1"}, \
    {.pin = 23, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "VGA HSync"}, \
    {.pin = 25, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "mono audio DAC"}, \
    {.pin = 26, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "PS/2 mouse clock"}, \
    {.pin = 27, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "PS/2 mouse data"}, \
    {.pin = 32, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "PS/2 keyboard data"}, \
    {.pin = 33, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "PS/2 keyboard clock"}, \
    {.pin = 34, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
    {.pin = 39, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "input-only expansion"}, \
}
