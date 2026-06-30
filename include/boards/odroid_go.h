#pragma once

#include "driver/gpio.h"
#include "driver/uart.h"
#include "solar_os_adc_dpad.h"
#include "solar_os_buttons.h"
#include "solar_os_keys.h"

#define SOLAR_OS_BOARD_ID "odroid_go"
#define SOLAR_OS_BOARD_NAME "Hardkernel ODROID-GO"
#define SOLAR_OS_BOARD_VENDOR "Hardkernel"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-WROVER"

#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3

#define SOLAR_OS_BOARD_PIN_TFT_DC GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_TFT_CS GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_TFT_LED GPIO_NUM_14
#define SOLAR_OS_BOARD_PIN_TFT_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_PIN_TFT_MISO GPIO_NUM_19
#define SOLAR_OS_BOARD_PIN_TFT_SCLK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_22

#define SOLAR_OS_BOARD_PIN_KEY GPIO_NUM_39
#define SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL 0
#define SOLAR_OS_BOARD_KEY_PULL_UP 0
#define SOLAR_OS_BOARD_KEY_PULL_DOWN 0

#define SOLAR_OS_BOARD_BUTTONS { \
    {.pin = GPIO_NUM_32, .name = "A", .key = '\n', .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_33, .name = "B", .key = 0x7f, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_13, .name = "MENU", .key = SOLAR_OS_KEY_ESCAPE, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_27, .name = "SELECT", .key = '\t', .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_UP}, \
    {.pin = GPIO_NUM_0, .name = "VOLUME", .key = SOLAR_OS_KEY_APP_EXIT, .active_low = true, .pull = SOLAR_OS_BUTTON_PULL_NONE}, \
}

#define SOLAR_OS_BOARD_ADC_DPAD_AXES { \
    {.pin = GPIO_NUM_34, .name = "x", .mid_key = SOLAR_OS_KEY_RIGHT, .high_key = SOLAR_OS_KEY_LEFT, .idle_max = 600, .mid_min = 1200, .mid_max = 2600, .high_min = 3300}, \
    {.pin = GPIO_NUM_35, .name = "y", .mid_key = SOLAR_OS_KEY_DOWN, .high_key = SOLAR_OS_KEY_UP, .idle_max = 600, .mid_min = 1200, .mid_max = 2600, .high_min = 3300}, \
}
