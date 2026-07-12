#include "pmic_axp192.h"

#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"

static const char *TAG = "pmic_axp192";

#define AXP192_I2C_ADDR 0x34

/* Power output enable register: bit1 = DCDC3 (Core2 LCD backlight rail),
 * bit2 = LDO2 (Core2 peripheral 3.3V rail). Read-modify-write so DCDC1
 * (the rail powering the ESP32 itself) is never touched. */
#define AXP192_REG_POWER_OUTPUT_CTRL 0x12
#define AXP192_POWER_DCDC3_BIT 0x02
#define AXP192_POWER_LDO2_BIT 0x04
/* EXTEN: AXP192's dedicated external-power-enable output, the more
 * common mechanism M5Stack's own ecosystem (M5StickC etc.) uses to
 * gate Grove/HAT power -- trying this alongside GPIO0 LDO below since
 * it's unconfirmed which one (if either) actually feeds Core2's Port A. */
#define AXP192_POWER_EXTEN_BIT 0x40

/* DCDC3 (backlight) output voltage: 0.7V + N * 0.025V, N in 0..127. */
#define AXP192_REG_DCDC3_VOLTAGE 0x27
#define AXP192_VOLTAGE_BASE_MV 700
#define AXP192_VOLTAGE_STEP_MV 25
#define AXP192_BACKLIGHT_TARGET_MV 3000

/* GPIO4 function/output registers used by the stock Core2 firmware to
 * hold the ILI9342C in reset (low) and release it (high). Confirm these
 * two bytes against the AXP192 datasheet GPIO3/4 block (or a schematic
 * capture) on first bring-up: if the panel stays blank but the backlight
 * rail above measures correctly on a meter, this is the pair to recheck.
 */
#define AXP192_REG_GPIO4_FUNCTION 0x95
#define AXP192_GPIO4_FUNCTION_NMOS_OUTPUT 0x84
#define AXP192_REG_GPIO34_SIGNAL 0x96
#define AXP192_GPIO4_OUTPUT_HIGH_BIT 0x02

/* GPIO0 configured as an LDO output feeds Grove Port A's 5V pin.
 * 0x02 = LDO mode (on), 0x07 = floating input (off). The LDO's own
 * output voltage is a separate register (0x91) that M5Unified sets
 * once during its own bring-up, not touched again by its runtime
 * on/off toggle -- without it the LDO has no configured voltage. */
#define AXP192_REG_GPIO0_FUNCTION 0x90
#define AXP192_GPIO0_LDO_ON 0x02
#define AXP192_REG_GPIO0_LDO_VOLTAGE 0x91
#define AXP192_GPIO0_LDO_VOLTAGE_2V8 0xA0
#define AXP192_GPIO0_FLOATING_OFF 0x07

static esp_err_t axp192_read(uint8_t reg, uint8_t *value)
{
    return i2c_bus_read_reg(AXP192_I2C_ADDR, reg, value, 1);
}

static esp_err_t axp192_write(uint8_t reg, uint8_t value)
{
    return i2c_bus_write_reg(AXP192_I2C_ADDR, reg, &value, 1);
}

static uint8_t axp192_dcdc3_voltage_code(uint16_t millivolts)
{
    if (millivolts < AXP192_VOLTAGE_BASE_MV) {
        return 0;
    }
    uint32_t steps = (millivolts - AXP192_VOLTAGE_BASE_MV) / AXP192_VOLTAGE_STEP_MV;
    if (steps > 0x7f) {
        steps = 0x7f;
    }
    return (uint8_t)steps;
}

esp_err_t pmic_axp192_core2_bringup(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(i2c_bus_probe(AXP192_I2C_ADDR), TAG, "AXP192 not found on i2c bus");

    uint8_t power_ctrl = 0;
    ESP_RETURN_ON_ERROR(axp192_read(AXP192_REG_POWER_OUTPUT_CTRL, &power_ctrl),
                        TAG,
                        "read power output ctrl failed");
    power_ctrl |= (AXP192_POWER_DCDC3_BIT | AXP192_POWER_LDO2_BIT);
    ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_POWER_OUTPUT_CTRL, power_ctrl),
                        TAG,
                        "enable DC3/LDO2 failed");

    const uint8_t voltage_code = axp192_dcdc3_voltage_code(AXP192_BACKLIGHT_TARGET_MV);
    ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_DCDC3_VOLTAGE, voltage_code),
                        TAG,
                        "set backlight voltage failed");

    ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_GPIO4_FUNCTION,
                                     AXP192_GPIO4_FUNCTION_NMOS_OUTPUT),
                        TAG,
                        "configure GPIO4 function failed");
    ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_GPIO34_SIGNAL,
                                     AXP192_GPIO4_OUTPUT_HIGH_BIT),
                        TAG,
                        "release LCD reset failed");

    /* ILI9342C needs some time coming out of reset before the first
     * command; the ili9341 driver's own post-reset delay covers the rest. */
    ESP_LOGI(TAG, "Core2 backlight/reset rails enabled (DC3=%umV)", AXP192_BACKLIGHT_TARGET_MV);
    return ESP_OK;
}

esp_err_t pmic_axp192_set_grove_a_power(bool enable)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");

    uint8_t power_ctrl = 0;
    ESP_RETURN_ON_ERROR(axp192_read(AXP192_REG_POWER_OUTPUT_CTRL, &power_ctrl),
                        TAG,
                        "read power output ctrl failed");
    if (enable) {
        power_ctrl |= AXP192_POWER_EXTEN_BIT;
    } else {
        power_ctrl &= (uint8_t)~AXP192_POWER_EXTEN_BIT;
    }
    ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_POWER_OUTPUT_CTRL, power_ctrl),
                        TAG,
                        "set EXTEN failed");

    if (enable) {
        ESP_RETURN_ON_ERROR(axp192_write(AXP192_REG_GPIO0_LDO_VOLTAGE, AXP192_GPIO0_LDO_VOLTAGE_2V8),
                            TAG,
                            "set GPIO0 LDO voltage failed");
    }
    return axp192_write(AXP192_REG_GPIO0_FUNCTION,
                        enable ? AXP192_GPIO0_LDO_ON : AXP192_GPIO0_FLOATING_OFF);
}
