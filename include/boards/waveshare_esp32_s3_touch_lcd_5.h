#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/uart.h"
#include "solar_os_expansion_types.h"

#define SOLAR_OS_BOARD_ID "waveshare_esp32_s3_touch_lcd_5"
#define SOLAR_OS_BOARD_NAME "Waveshare ESP32-S3-Touch-LCD-5"
#define SOLAR_OS_BOARD_VENDOR "Waveshare"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3-WROOM-1-N16R8"

#define SOLAR_OS_BOARD_CAPABILITIES \
    (SOLAR_OS_BOARD_CAP_PSRAM | \
     SOLAR_OS_BOARD_CAP_SIMD | \
     SOLAR_OS_BOARD_CAP_DISPLAY | \
     SOLAR_OS_BOARD_CAP_GFX | \
     SOLAR_OS_BOARD_CAP_CDC | \
     SOLAR_OS_BOARD_CAP_UART | \
     SOLAR_OS_BOARD_CAP_SD | \
     SOLAR_OS_BOARD_CAP_I2C | \
     SOLAR_OS_BOARD_CAP_SPI | \
     SOLAR_OS_BOARD_CAP_RTC | \
     SOLAR_OS_BOARD_CAP_WIFI | \
     SOLAR_OS_BOARD_CAP_BLE)

/*
 * 800x480 RGB-parallel panel (ST7262 controller, no command interface --
 * "simple RGB", so there's no SPI/I2C init sequence to send, just the
 * timing below plus reset/backlight through the CH422G expander).
 * Values below are Waveshare's own reference config for this exact
 * SKU (28117), from esp-arduino-libs/ESP32_Display_Panel's
 * BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_5.h -- not re-derived/guessed.
 */
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7262"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 800
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 480

#define SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_HZ (16 * 1000 * 1000)
#define SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_PULSE 4
#define SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_BACK_PORCH 8
#define SOLAR_OS_BOARD_DISPLAY_RGB_HSYNC_FRONT_PORCH 8
#define SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_PULSE 4
#define SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_BACK_PORCH 8
#define SOLAR_OS_BOARD_DISPLAY_RGB_VSYNC_FRONT_PORCH 8
#define SOLAR_OS_BOARD_DISPLAY_RGB_PCLK_ACTIVE_NEG 1
#define SOLAR_OS_BOARD_DISPLAY_RGB_DISP_ACTIVE_LEVEL 1

#define SOLAR_OS_BOARD_PIN_LCD_RGB_HSYNC GPIO_NUM_46
#define SOLAR_OS_BOARD_PIN_LCD_RGB_VSYNC GPIO_NUM_3
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DE GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_LCD_RGB_PCLK GPIO_NUM_7

/* RGB565 over 16 data lines: B0-4, G0-5, R0-4. */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA0 GPIO_NUM_14  /* B0 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA1 GPIO_NUM_38  /* B1 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA2 GPIO_NUM_18  /* B2 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA3 GPIO_NUM_17  /* B3 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA4 GPIO_NUM_10  /* B4 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA5 GPIO_NUM_39  /* G0 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA6 GPIO_NUM_0   /* G1 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA7 GPIO_NUM_45  /* G2 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA8 GPIO_NUM_48  /* G3 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA9 GPIO_NUM_47  /* G4 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA10 GPIO_NUM_21 /* G5 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA11 GPIO_NUM_1  /* R0 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA12 GPIO_NUM_2  /* R1 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA13 GPIO_NUM_42 /* R2 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA14 GPIO_NUM_41 /* R3 */
#define SOLAR_OS_BOARD_PIN_LCD_RGB_DATA15 GPIO_NUM_40 /* R4 */

/*
 * CH422G IO expander pin indices (its 8 push-pull GPIOs, IO0-IO7 --
 * not raw ESP32 GPIO numbers). Confirmed from Waveshare's docs and the
 * same ESP32_Display_Panel board config: TP_RST=1, DISP(backlight)=2,
 * LCD_RST=3, SD_CS=4. DI0/DI1 (0/5) are inputs Waveshare reserves for
 * future use and aren't driven by this port.
 */
#define SOLAR_OS_BOARD_CH422G_PIN_TP_RST 1
#define SOLAR_OS_BOARD_CH422G_PIN_BACKLIGHT 2
#define SOLAR_OS_BOARD_CH422G_PIN_LCD_RST 3
#define SOLAR_OS_BOARD_CH422G_PIN_SD_CS 4

/*
 * Internal I2C bus: CH422G expander, GT911 touch (not driven by this
 * port yet) and PCF85063 RTC all share this bus.
 */
#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_8
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_9

/*
 * Dedicated SPI bus for the SD card only (the display doesn't use SPI
 * at all -- it's RGB parallel). CS is driven through the CH422G
 * expander rather than a raw GPIO (see lcd_rgb_panel.c, which asserts
 * it once at display-init time), so SOLAR_OS_BOARD_PIN_SD_CARD_CS is
 * GPIO_NUM_NC and the SPI driver never toggles hardware CS itself.
 *
 * UNVERIFIED: Waveshare's docs only give "SD Card: SPI on GPIO11-13"
 * without saying which pin is which signal. This SCLK/MOSI/MISO
 * assignment is a best guess pending confirmation on real hardware --
 * check against the schematic PDF or a continuity meter if the card
 * doesn't mount.
 */
#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "SD_SPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_11
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_13
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_NC

/*
 * UART lives on GPIO15/16 -- the board's stock CAN pins (TJA1051
 * TXD/RXD). Confirmed from the schematic: GPIO15=CANTX, GPIO16=CANRX.
 * ESP32-S3 UART controllers are fully GPIO-matrix routable, so this
 * works in software regardless of pin choice, but electrically these
 * only become a genuine point-to-point serial port once the TJA1051
 * is swapped for a MAX232 (or similar) on the hardware side -- until
 * then, whatever's driven here still passes through the CAN
 * transceiver as originally wired. Swapping the transceiver trades
 * away CAN support for a real UART/RS232 port. UART_NUM_1 is used
 * (not 0) purely to keep clear of any ROM/bootloader UART0 defaults;
 * nothing on this board is actually wired to UART0's pins.
 */
#define SOLAR_OS_BOARD_UART_PORT UART_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_15
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_16

/*
 * Minimal port: display, RTC, SD, UART, connectivity. Not yet wired:
 * GT911 touch (SolarOS has no touch-input capability/service at all
 * yet -- follow-up work, same as Core2's touch) and RS485 (SP3485 on
 * GPIO43/44, which are also the ESP32-S3's default UART0 pins --
 * SolarOS's UART service only supports one active instance at a time,
 * so RS485 would need its own follow-up work rather than just being
 * wired alongside the GPIO15/16 UART above). USB native CDC
 * (GPIO19/20) is also available as a console.
 */
