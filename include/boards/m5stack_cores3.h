#pragma once

#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2s_types.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "solar_os_expansion_types.h"

#define SOLAR_OS_BOARD_ID "m5stack_cores3"
#define SOLAR_OS_BOARD_NAME "M5Stack CoreS3"
#define SOLAR_OS_BOARD_VENDOR "M5Stack"
#define SOLAR_OS_BOARD_MODULE_NAME "ESP32-S3"

#define SOLAR_OS_BOARD_CAPABILITIES \
    (SOLAR_OS_BOARD_CAP_PSRAM | \
     SOLAR_OS_BOARD_CAP_SIMD | \
     SOLAR_OS_BOARD_CAP_DISPLAY | \
     SOLAR_OS_BOARD_CAP_GFX | \
     SOLAR_OS_BOARD_CAP_CDC | \
     SOLAR_OS_BOARD_CAP_UART | \
     SOLAR_OS_BOARD_CAP_I2C | \
     SOLAR_OS_BOARD_CAP_SPI | \
     SOLAR_OS_BOARD_CAP_RTC | \
     SOLAR_OS_BOARD_CAP_WIFI | \
     SOLAR_OS_BOARD_CAP_BLE | \
     SOLAR_OS_BOARD_CAP_AUDIO)

/*
 * ILI9342C, 320x240 -- same command family/controller as Core2, so
 * MADCTL/rotation below start from Core2's proven-correct values for
 * this exact chip (native GRAM portrait, needs a software 90 degree
 * rotation) rather than the SPI_FREQUENCY=40000000/orientation=6
 * TFT_eSPI values from the reference firmware this port was checked
 * against (github.com/lshaf/unigeek, boards/m5_cores3) -- TFT_eSPI's
 * rotation table doesn't map directly to a raw MADCTL byte here.
 * CoreS3 is a different physical product/mounting than Core2 though,
 * so this is a starting point, not a confirmed value -- verify on
 * hardware and adjust like Core2's port needed.
 */
#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ILI9342C"
#define SOLAR_OS_BOARD_DISPLAY_WIDTH 320
#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 240
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 320
#define SOLAR_OS_BOARD_DISPLAY_SPI_CLOCK_HZ 40000000
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0xa8
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R1
/* M5Stack's ILI9342C panels need color inversion enabled (INVON) or
 * everything renders in negative -- spotted by comparing against the
 * remote job's framebuffer view, which showed the true logical colors. */
#define SOLAR_OS_BOARD_DISPLAY_INVERT 1

/*
 * SPI bus is LCD-only in this port (SD is deferred -- see the .cmake
 * comment on why: GPIO35 does double duty as LCD DC and SD MISO on
 * this board, switched dynamically per-operation in the reference
 * firmware, which solar_os's shared SPI/display/SD code has no
 * mechanism for yet). No MISO is wired since nothing on this bus needs
 * to read back.
 */
#define SOLAR_OS_BOARD_SPI_HOST SPI2_HOST
#define SOLAR_OS_BOARD_SPI_NAME "LCD_SPI"
#define SOLAR_OS_BOARD_PIN_SPI_SCLK GPIO_NUM_36
#define SOLAR_OS_BOARD_PIN_SPI_MOSI GPIO_NUM_37
#define SOLAR_OS_BOARD_PIN_SPI_MISO GPIO_NUM_NC
#define SOLAR_OS_BOARD_SPI_MAX_TRANSFER_SZ 4096

#define SOLAR_OS_BOARD_PIN_LCD_DC GPIO_NUM_35
#define SOLAR_OS_BOARD_PIN_LCD_CS GPIO_NUM_3
#define SOLAR_OS_BOARD_PIN_LCD_MOSI SOLAR_OS_BOARD_PIN_SPI_MOSI
#define SOLAR_OS_BOARD_PIN_LCD_SCK SOLAR_OS_BOARD_PIN_SPI_SCLK
/*
 * No PIN_LCD_RST/_TE/_BL defined: reset is behind the AW9523B expander
 * (P1.5) and backlight is behind the AXP2101 PMIC (DLDO1), neither a
 * raw ESP32 GPIO -- see io_expander_aw9523b.c/pmic_axp2101.c, driven
 * once at display-init time in solar_os_board_display_ili9341.c.
 */

/*
 * Internal I2C bus: AXP2101 PMIC, AW9523B expander, BM8563 RTC, and
 * AW88298 audio out all share this bus. FT6336U touch and ES7210 audio
 * in (not yet driven by this port) are also on it.
 */
#define SOLAR_OS_BOARD_I2C_PORT I2C_NUM_0
#define SOLAR_OS_BOARD_PIN_I2C_SDA GPIO_NUM_12
#define SOLAR_OS_BOARD_PIN_I2C_SCL GPIO_NUM_11

/*
 * Grove Port A is its own separate I2C controller/pins, NOT the
 * internal bus above. drivers/i2c_bus_port_a.c owns this bus, shared
 * by CardKB and anything else that gets wired up here. SDA=2/SCL=1
 * confirmed twice: M5Unified's _pin_table_i2c_ex_in AND an Arduino
 * scan test on this exact unit (Wire.begin(2, 1) found the CardKB at
 * 0x5f; the swapped order did not).
 */
#define SOLAR_OS_BOARD_PORT_A_I2C_PORT I2C_NUM_1
#define SOLAR_OS_BOARD_PIN_PORT_A_I2C_SDA GPIO_NUM_2
#define SOLAR_OS_BOARD_PIN_PORT_A_I2C_SCL GPIO_NUM_1

/* USB native CDC (Type-C, OTG+CDC) console, plus plain UART0 on its
 * default ESP32-S3 pins -- unlike ESP32-S3-Touch-LCD-5, nothing else
 * on this board claims these pins, so both consoles are available with
 * no caveats. */
#define SOLAR_OS_BOARD_UART_PORT UART_NUM_0
#define SOLAR_OS_BOARD_PIN_UART_TX GPIO_NUM_43
#define SOLAR_OS_BOARD_PIN_UART_RX GPIO_NUM_44

/*
 * Default UART for the dhex/aqm apps' particle sensor. Unlike Core2
 * (GPIO13/14, free there), CoreS3's 13/14 belong to I2S audio -- so
 * the default here is Grove Port C, the board's dedicated UART port
 * (pinmap: "PORT.C | GND | 5V | G17 | G18", G17=PC_TX, G18=PC_RX).
 * Its 5V rail comes from the same SY7088 boost as Port A, already on
 * since expander init. dhex can override all of this at runtime.
 */
#define SOLAR_OS_BOARD_PM_UART_PORT UART_NUM_1
#define SOLAR_OS_BOARD_PIN_PM_UART_TX GPIO_NUM_17
#define SOLAR_OS_BOARD_PIN_PM_UART_RX GPIO_NUM_18

/*
 * Speaker: AW88298 class-D amp over I2S1 + I2C (shared internal bus
 * above). Pins/port cross-checked against M5Stack's own M5Unified
 * driver (M5Unified.cpp board_M5StackCoreS3 case), not just the
 * unigeek reference repo: BCK=IO34, WS=IO33, DOUT=IO13, I2S_NUM_1, no
 * MCLK wired to the amp. Amp power is gated by the AW9523B expander's
 * P0.2, not a raw GPIO (io_expander_aw9523b_set_speaker_enable()), so
 * PIN_AUDIO_PA is left unset. Mic (ES7210, IO14 DIN, same BCK/WS) is a
 * separate codec sharing this bus -- not wired by this port yet.
 */
#define SOLAR_OS_BOARD_I2S_PORT I2S_NUM_1
#define SOLAR_OS_BOARD_PIN_I2S_MCLK GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_I2S_BCLK GPIO_NUM_34
#define SOLAR_OS_BOARD_PIN_I2S_WS GPIO_NUM_33
#define SOLAR_OS_BOARD_PIN_I2S_DIN GPIO_NUM_NC
#define SOLAR_OS_BOARD_PIN_I2S_DOUT GPIO_NUM_13
#define SOLAR_OS_BOARD_AUDIO_CODEC_OUT "AW88298"
#define SOLAR_OS_BOARD_AUDIO_CODEC_IN "none"
