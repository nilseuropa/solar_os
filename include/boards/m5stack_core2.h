#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_expansion_types.h"

#define SOLAR_OS_BOARD_ID "m5stack_core2"
#define SOLAR_OS_BOARD_NAME "M5Stack Core2"
#define SOLAR_OS_BOARD_VENDOR "M5Stack"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-D0WDQ6"

/* USB-UART bridge (CP2104/CH9102) on the classic ESP32 UART0 pins. */
#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_1
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_3

/*
 * Dedicated second UART for the particle sensor (Serial2 on the
 * original project this was ported from) -- a separate hardware UART
 * controller from the console above, not shared with it. Read raw by
 * the dhex app, parsed into PM1.0/2.5/10 by the aqm app; both talk to
 * the same physical sensor on this port. GPIO13/14 are free on Core2
 * (no display/PMIC/touch/IMU claims them). Every board that wants
 * either app declares its own SOLAR_OS_BOARD_PM_UART_* trio, since
 * which UART controller is free and which pins are available varies
 * per board (e.g. CoreS3's GPIO13 is already the AW88298 speaker's
 * I2S DOUT).
 */
#define SOLAR_OS_BOARD_PM_UART_PORT UART_NUM_2
#define SOLAR_OS_BOARD_PIN_PM_UART_TX GPIO_NUM_14
#define SOLAR_OS_BOARD_PIN_PM_UART_RX GPIO_NUM_13

/*
 * ILI9342C, 320x240, same command family as the ILI9341 driver already
 * used by odroid_go. MADCTL/rotation below reproduce the widely used
 * Core2 bring-up values (landscape, BGR order). Verify against your
 * physical unit and adjust if the image comes up mirrored or rotated;
 * see doc comment near SOLAR_OS_BOARD_DISPLAY_MADCTL below.
 */
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ILI9342C"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 240
/*
 * tft_ili9341.c always sends column range 0..NATIVE_WIDTH-1 via CASET
 * (0x2A) and page range 0..NATIVE_HEIGHT-1 via PASET (0x2B); those bounds
 * have to match whatever MADCTL_MV says the controller's GRAM is
 * addressed as. Panel_ILI9342's un-rotated GRAM is 320x240 (confirmed
 * from M5GFX's Panel_ILI9342 constructor), so MV=0 wants native 320x240
 * and MV=1 wants native 240x320 -- mixing MV=1 with un-swapped 320x240
 * bounds (or vice versa) sends out-of-range addresses and the panel
 * wraps, which shows up as a doubled/mirrored image.
 *
 * Bringing the panel up with MV=0 (plain BGR, MADCTL=0x08) produced a
 * clean, non-corrupted image on this unit but rotated 90 degrees
 * clockwise -- so the module is physically mounted rotated relative to
 * the controller's MV=0 default, and a true MV=1 rotation (with matching
 * swapped native dims, same pairing odroid_go uses for its ILI9341) is
 * needed rather than a plain MX/MY mirror.
 */
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 320
/*
 * Core2's VSPI MISO is wired to GPIO38, not the SPI3_HOST native IOMUX
 * pin (GPIO19), so the SPI driver has to route through the GPIO matrix.
 * That caps the usable clock well below the 40MHz odroid_go can hit on
 * IOMUX pins -- ESP-IDF rejects anything >= 26.67MHz here
 * (spi_hal: "clock_speed_hz should less than 26666666").
 */
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 20000000
/*
 * MV performs a row/column transpose, which is a reflection (odd
 * parity); MX and MY are each a single-axis reflection too. MV alone
 * (0x28: upside-down + mirrored) and MV|MX|MY (0xe8: right-side-up but
 * still mirrored) are both net reflections. MV|MX (0x68) fixed the
 * mirror but came up upside-down, so MV|MY is the remaining candidate --
 * confirmed correct: right-side-up, landscape, no mirroring.
 */
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0xa8
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1

/*
 * Shared VSPI bus: LCD and SD card both hang off it, same pattern as
 * odroid_go, just with Core2's pinout (MISO is GPIO38 here, not 19).
 */
#define SOLAR_OS_BOARD_SPI_HOST SPI3_HOST
#define SOLAR_OS_BOARD_SPI_NAME "VSPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_38
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096

#define SOLAR_OS_BOARD_PIN_TFT_DC GPIO_NUM_15
#define SOLAR_OS_BOARD_PIN_TFT_CS GPIO_NUM_5
#define SOLAR_OS_BOARD_PIN_TFT_MOSI GPIO_NUM_23
#define SOLAR_OS_BOARD_PIN_TFT_MISO GPIO_NUM_38
#define SOLAR_OS_BOARD_PIN_TFT_SCLK GPIO_NUM_18
#define SOLAR_OS_BOARD_PIN_SD_CARD_CS GPIO_NUM_4

#define SOLAR_OS_BOARD_PIN_LCD_DC SOLAR_OS_BOARD_PIN_TFT_DC
#define SOLAR_OS_BOARD_PIN_LCD_CS SOLAR_OS_BOARD_PIN_TFT_CS
#define SOLAR_OS_BOARD_PIN_LCD_SCK SOLAR_OS_BOARD_PIN_TFT_SCLK
#define SOLAR_OS_BOARD_PIN_LCD_MOSI SOLAR_OS_BOARD_PIN_TFT_MOSI
#define SOLAR_OS_BOARD_PIN_LCD_MISO SOLAR_OS_BOARD_PIN_TFT_MISO
/*
 * Core2 does not wire LCD RESET or LCD backlight to any ESP32 GPIO: both
 * are behind the AXP192 PMIC (GPIO4 for reset, DC3 for backlight). Leave
 * these undefined so the shared ILI9341 driver skips its GPIO reset and
 * PWM-backlight paths entirely (it already supports that, guarded by
 * #ifdef). The AXP192 bring-up in drivers/pmic_axp192.c does both jobs
 * over I2C before the panel init sequence runs.
 */

/*
 * Internal I2C bus: AXP192 PMIC, FT6336U touch, BM8563 RTC and MPU6886
 * IMU all share this bus at SDA=GPIO21 / SCL=GPIO22. PMIC and RTC are
 * driven for this port; touch/IMU are follow-up work.
 */
#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_21
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_22

/*
 * Port A (external I2C, SDA=GPIO32/SCL=GPIO33) is not wired up as a
 * generic expansion bus yet (add SOLAR_OS_BOARD_EXPANSION_I2C_BUSES
 * plus the EXPANSION_I2C capability in m5stack_core2.cmake for that).
 * drivers/i2c_bus_port_a.c owns this as its own I2C_NUM_1 instance,
 * shared by both solar_os_cardkb.c (CardKB) and the aqm app (SCD41 CO2
 * sensor) -- multiple I2C devices can share one bus at different
 * addresses, they just can't each call i2c_new_master_bus() on the
 * same port. Port A's 5V pin is NOT a fixed rail (an earlier version
 * of this comment incorrectly claimed that) -- it's AXP192 GPIO0
 * configured as an LDO output, off by default; see
 * pmic_axp192_set_grove_a_power().
 */
#define SOLAR_OS_BOARD_PORT_A_I2C_PORT I2C_NUM_1
#define SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA GPIO_NUM_32
#define SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL GPIO_NUM_33
