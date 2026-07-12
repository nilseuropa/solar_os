#include "io_expander_aw9523b.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"

static const char *TAG = "aw9523b";

#define AW9523B_I2C_ADDR 0x58

#define AW9523B_REG_OUTPUT_P0 0x02
#define AW9523B_REG_OUTPUT_P1 0x03
#define AW9523B_REG_DIR_P0 0x04
#define AW9523B_REG_DIR_P1 0x05
#define AW9523B_REG_GCR 0x11
#define AW9523B_REG_LED_MODE_P0 0x12
#define AW9523B_REG_LED_MODE_P1 0x13
#define AW9523B_REG_SOFT_RESET 0x7f

/* Port 0: P0.0 = touch reset, P0.1 = Grove A BUS_EN, P0.2 = speaker amp
 * enable, P0.3-P0.4 = input, P0.5-P0.7 = output (unused, floated high).
 * Deasserted/idle state: touch reset released (1), BUS_EN on (1), amp
 * off (0). */
#define AW9523B_P0_OUTPUT_INIT 0x03
#define AW9523B_P0_DIR 0x18 /* 0=output,1=input: bits 3-4 input, rest output */

/* Port 1: P1.0-P1.1 must stay high (internal bus enables), P1.2-P1.3 =
 * input, P1.4/P1.6 = output (unused), P1.5 = LCD reset (starts
 * asserted low here, released separately below), P1.7 = Grove A
 * BOOST_EN -- on from init, matching M5GFX's own CoreS3 bring-up
 * (0b10000011). Since this init soft-resets the expander on every
 * boot (cutting Port A's 5V), enabling boost here instead of waiting
 * for a peripheral driver to ask gives whatever's plugged into Port A
 * (CardKB's MCU needs a few hundred ms to boot) the whole SolarOS
 * startup window to come up before anything probes it. */
#define AW9523B_P1_OUTPUT_INIT 0x83
#define AW9523B_P1_DIR 0x0c /* 0=output,1=input: bits 2-3 input, rest output */
#define AW9523B_P1_LCD_RST_BIT 0x20

#define AW9523B_P0_SPEAKER_EN_BIT 0x04
#define AW9523B_P1_GROVE_A_BOOST_EN_BIT 0x80

static esp_err_t aw9523b_write(uint8_t reg, uint8_t value)
{
    return i2c_bus_write_reg(AW9523B_I2C_ADDR, reg, &value, 1);
}

static esp_err_t aw9523b_read(uint8_t reg, uint8_t *value)
{
    return i2c_bus_read_reg(AW9523B_I2C_ADDR, reg, value, 1);
}

esp_err_t io_expander_aw9523b_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(i2c_bus_probe(AW9523B_I2C_ADDR), TAG, "AW9523B not found on i2c bus");

    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_SOFT_RESET, 0x00), TAG, "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Output values first, so no pin glitches low when direction
     * switches to output. */
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_OUTPUT_P0, AW9523B_P0_OUTPUT_INIT),
                        TAG,
                        "port0 output init failed");
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_OUTPUT_P1, AW9523B_P1_OUTPUT_INIT),
                        TAG,
                        "port1 output init failed");

    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_GCR, 0x10), TAG, "gcr config failed");
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_LED_MODE_P0, 0xff), TAG, "port0 gpio mode failed");
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_LED_MODE_P1, 0xff), TAG, "port1 gpio mode failed");

    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_DIR_P0, AW9523B_P0_DIR), TAG, "port0 direction failed");
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_DIR_P1, AW9523B_P1_DIR), TAG, "port1 direction failed");
    vTaskDelay(pdMS_TO_TICKS(8));

    /* Release LCD reset: P1.5 high, leave everything else as set above. */
    uint8_t port1 = 0;
    ESP_RETURN_ON_ERROR(aw9523b_read(AW9523B_REG_OUTPUT_P1, &port1), TAG, "port1 output read failed");
    port1 |= AW9523B_P1_LCD_RST_BIT;
    ESP_RETURN_ON_ERROR(aw9523b_write(AW9523B_REG_OUTPUT_P1, port1), TAG, "lcd reset release failed");
    vTaskDelay(pdMS_TO_TICKS(64));

    ESP_LOGI(TAG, "CoreS3 expander ready, LCD reset released");
    return ESP_OK;
}

esp_err_t io_expander_aw9523b_set_speaker_enable(bool enable)
{
    uint8_t port0 = 0;
    ESP_RETURN_ON_ERROR(aw9523b_read(AW9523B_REG_OUTPUT_P0, &port0), TAG, "port0 output read failed");
    if (enable) {
        port0 |= AW9523B_P0_SPEAKER_EN_BIT;
    } else {
        port0 &= (uint8_t)~AW9523B_P0_SPEAKER_EN_BIT;
    }
    return aw9523b_write(AW9523B_REG_OUTPUT_P0, port0);
}

esp_err_t io_expander_aw9523b_set_grove_a_boost_enable(bool enable)
{
    uint8_t port1 = 0;
    ESP_RETURN_ON_ERROR(aw9523b_read(AW9523B_REG_OUTPUT_P1, &port1), TAG, "port1 output read failed");
    if (enable) {
        port1 |= AW9523B_P1_GROVE_A_BOOST_EN_BIT;
    } else {
        port1 &= (uint8_t)~AW9523B_P1_GROVE_A_BOOST_EN_BIT;
    }
    return aw9523b_write(AW9523B_REG_OUTPUT_P1, port1);
}
