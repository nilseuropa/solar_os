#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_buttons.h"
#include "solar_os_bus_types.h"
#include "solar_os_keys.h"
#include "solar_os_pin_types.h"
#include "solar_os_spi.h"

#define SOLAR_OS_BOARD_ID "odroid_go"
#define SOLAR_OS_BOARD_NAME "Hardkernel ODROID-GO"
#define SOLAR_OS_BOARD_VENDOR "Hardkernel"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-WROVER"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3
#define SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK ((1U << UART_NUM_1) | (1U << UART_NUM_2))

#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ILI9341"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 320
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0x88
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1

#define SOLAR_OS_BOARD_SPI_HOST SPI3_HOST
#define SOLAR_OS_BOARD_SPI_NAME "VSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_19
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_SPI_CS_SLOTS { \
    {.pin = GPIO_NUM_15, .name = "io15"}, \
    {.pin = GPIO_NUM_4, .name = "io4"}, \
}
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
            .cs_count = 2, \
            .cs = { \
                {.name = "io15", .pin = GPIO_NUM_15}, \
                {.name = "io4", .pin = GPIO_NUM_4}, \
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
}

#define SOLAR_OS_BOARD_PIN_TFT_DC GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_TFT_CS GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_TFT_LED GPIO_NUM_14
#define SOLAR_OS_BOARD_PIN_TFT_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_PIN_TFT_MISO GPIO_NUM_19
#define SOLAR_OS_BOARD_PIN_TFT_SCLK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_22

#define SOLAR_OS_BOARD_PIN_BATTERY_ADC GPIO_NUM_36
#define SOLAR_OS_BOARD_BATTERY_ADC_DIVIDER_RATIO 2.0f

#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT 1
#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES { \
    { \
        .driver = "battery-adc", \
        .name = "battery0", \
        .binding_count = 2, \
        .bindings = { \
            {.kind = SOLAR_OS_EXPANSION_BINDING_ADC, .role = "battery", .value = SOLAR_OS_BOARD_PIN_BATTERY_ADC}, \
            {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "divider", .value = 2000}, \
        }, \
    }, \
}

#define SOLAR_OS_BOARD_PIN_AUDIO_AMP_EN GPIO_NUM_25
#define SOLAR_OS_BOARD_AUDIO_AMP_EN_ACTIVE_LEVEL 1
#define SOLAR_OS_BOARD_PIN_AUDIO_DAC_POS GPIO_NUM_26
#define SOLAR_OS_BOARD_PIN_AUDIO_DAC_NEG GPIO_NUM_NC
#define SOLAR_OS_BOARD_AUDIO_CODEC_OUT "ESP32-DAC"
#define SOLAR_OS_BOARD_AUDIO_CODEC_IN "-"

#define SOLAR_OS_BOARD_PIN_STATUS_LED GPIO_NUM_2
#define SOLAR_OS_BOARD_STATUS_LED_ACTIVE_LEVEL 1

#define SOLAR_OS_BOARD_PIN_LCD_DC SOLAR_OS_BOARD_PIN_TFT_DC
#define SOLAR_OS_BOARD_PIN_LCD_CS SOLAR_OS_BOARD_PIN_TFT_CS
#define SOLAR_OS_BOARD_PIN_LCD_SCK SOLAR_OS_BOARD_PIN_TFT_SCLK
#define SOLAR_OS_BOARD_PIN_LCD_MOSI SOLAR_OS_BOARD_PIN_TFT_MOSI
#define SOLAR_OS_BOARD_PIN_LCD_MISO SOLAR_OS_BOARD_PIN_TFT_MISO
#define SOLAR_OS_BOARD_PIN_LCD_RST GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_LCD_TE GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_LCD_BL SOLAR_OS_BOARD_PIN_TFT_LED
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_ACTIVE_LEVEL 1
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM 1
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PWM_FREQ_HZ 20000U

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_39
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 0
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_BUTTONS { \
    {.pin = GPIO_NUM_32, .name = "A", .key = '\n', .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_33, .name = "B", .key = ' ', .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_13, .name = "MENU", .key = SOLAR_OS_KEY_APP_EXIT, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_27, .name = "SELECT", .key = SOLAR_OS_KEY_ESCAPE, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_0, .name = "VOLUME", .key = SOLAR_OS_KEY_AUDIO_MUTE_TOGGLE, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
}

#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE "P2 external I/O header"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_VIEW \
    "component side; header pin 1 to pin 10"
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_ROWS 1
#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_COLUMNS 10
#define SOLAR_OS_BOARD_CONNECTOR_PIN_COUNT 10
#define SOLAR_OS_BOARD_CONNECTOR_PINS { \
    {.connector = "P2", .position = 1, .row = 0, .column = 0, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_GROUND, .label = "GND"}, \
    {.connector = "P2", .position = 2, .row = 0, .column = 1, .pin = GPIO_NUM_18, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "SCLK"}, \
    {.connector = "P2", .position = 3, .row = 0, .column = 2, .pin = GPIO_NUM_15, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO15"}, \
    {.connector = "P2", .position = 4, .row = 0, .column = 3, .pin = GPIO_NUM_4, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO4"}, \
    {.connector = "P2", .position = 5, .row = 0, .column = 4, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "3V3"}, \
    {.connector = "P2", .position = 6, .row = 0, .column = 5, .pin = GPIO_NUM_19, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "MISO"}, \
    {.connector = "P2", .position = 7, .row = 0, .column = 6, .pin = GPIO_NUM_12, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "IO12"}, \
    {.connector = "P2", .position = 8, .row = 0, .column = 7, .pin = GPIO_NUM_23, .kind = SOLAR_OS_CONNECTOR_PIN_GPIO, .label = "MOSI"}, \
    {.connector = "P2", .position = 9, .row = 0, .column = 8, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_NC, .label = "NC"}, \
    {.connector = "P2", .position = 10, .row = 0, .column = 9, .pin = -1, .kind = SOLAR_OS_CONNECTOR_PIN_POWER, .label = "VBUS"}, \
}

#define SOLAR_OS_BOARD_ADC_DPAD_AXES { \
    {.pin = GPIO_NUM_34, .name = "x", .mid_key = SOLAR_OS_KEY_RIGHT, .high_key = SOLAR_OS_KEY_LEFT, .idle_max = 600, .mid_min = 1200, .mid_max = 2600, .high_min = 3300}, \
    {.pin = GPIO_NUM_35, .name = "y", .mid_key = SOLAR_OS_KEY_DOWN, .high_key = SOLAR_OS_KEY_UP, .idle_max = 600, .mid_min = 1200, .mid_max = 2600, .high_min = 3300}, \
}

#define SOLAR_OS_BOARD_EXPANSION_GPIO_MASK ((1ULL << GPIO_NUM_4) | \
                                            (1ULL << GPIO_NUM_15))
#define SOLAR_OS_BOARD_USER_GPIO_MASK SOLAR_OS_BOARD_EXPANSION_GPIO_MASK
#define SOLAR_OS_BOARD_EXPANSION_GPIO_LIST "4 15"
#define SOLAR_OS_BOARD_USER_GPIO_LIST "4 15"
#define SOLAR_OS_BOARD_EXPANSION_PWM_MASK SOLAR_OS_BOARD_USER_GPIO_MASK
#define SOLAR_OS_BOARD_GPIO_SLOTS { \
    {.pin = 2, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "status LED"}, \
    {.pin = 14, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "LCD backlight"}, \
    {.pin = 25, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "speaker enable"}, \
    {.pin = 26, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "speaker DAC"}, \
    {.pin = 36, .policy = SOLAR_OS_PIN_POLICY_FIXED, .role = "battery ADC"}, \
    {.pin = 4, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "external IO / SPI CS"}, \
    {.pin = 15, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = "external IO / SPI CS"}, \
}
