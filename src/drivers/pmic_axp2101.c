#include "pmic_axp2101.h"

#include "esp_check.h"
#include "esp_log.h"
#include "i2c_bus.h"

static const char *TAG = "pmic_axp2101";

#define AXP2101_I2C_ADDR 0x34

#define AXP2101_REG_ADC_ENABLE 0x30
#define AXP2101_ADC_ENABLE_ALL 0x0f

#define AXP2101_REG_POWERKEY 0x27
#define AXP2101_POWERKEY_HOLD1S_OFF4S 0x00

#define AXP2101_REG_PMU_CONFIG 0x10
#define AXP2101_PMU_CONFIG_DEFAULT 0x30

#define AXP2101_REG_CHGLED 0x69
#define AXP2101_CHGLED_DEFAULT 0x11

/* LDO voltage registers, in 100mV steps from a chip-specific base; the
 * raw codes below are the values M5Stack's own firmware uses for these
 * exact rails, not independently derived. */
#define AXP2101_REG_ALDO1_VOLTAGE 0x92
#define AXP2101_REG_ALDO2_VOLTAGE 0x93
#define AXP2101_REG_ALDO3_VOLTAGE 0x94
#define AXP2101_REG_ALDO4_VOLTAGE 0x95
#define AXP2101_REG_BLDO1_VOLTAGE 0x96
#define AXP2101_REG_DLDO1_VOLTAGE 0x99
#define AXP2101_ALDO1_1V8 13
#define AXP2101_LDO_3V3 28

#define AXP2101_REG_POWER_OUTPUT_CTRL 0x90
#define AXP2101_POWER_ALDO4_BIT 0x01
#define AXP2101_POWER_ALDO3_BIT 0x02
#define AXP2101_POWER_ALDO2_BIT 0x04
#define AXP2101_POWER_ALDO1_BIT 0x08
/* Unidentified rail (BLDO2? CPUSLDO?) -- M5Unified's own CoreS3
 * bring-up sequence writes 0x90=0xBF unconditionally at boot, which
 * includes this bit; the existing ALDO1-4/BLDO1/DLDO1 bits above
 * already account for the rest of that byte. Not independently
 * confirmed which physical rail this is, but M5Unified always enables
 * it, so mirroring that rather than guessing it's safe to omit. */
#define AXP2101_POWER_UNKNOWN_BIT4 0x10
#define AXP2101_POWER_BLDO1_BIT 0x20
#define AXP2101_POWER_DLDO1_BIT 0x80

/* DLDO1 feeds the SY7088 boost converter that drives the LCD backlight
 * LEDs. Formula matches M5GFX's Light_M5StackCoreS3: percent is scaled
 * to 0-255, then mapped into the chip's 20-28 code range (~2.5-3.3V);
 * below ~2.7V the boost converter loses regulation, giving a crude dim
 * effect at low settings rather than a linear one. */
#define AXP2101_DLDO1_CODE_OFFSET 641
#define AXP2101_DLDO1_CODE_SHIFT 5

static esp_err_t axp2101_write(uint8_t reg, uint8_t value)
{
    return i2c_bus_write_reg(AXP2101_I2C_ADDR, reg, &value, 1);
}

esp_err_t pmic_axp2101_cores3_bringup(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "i2c bus init failed");
    ESP_RETURN_ON_ERROR(i2c_bus_probe(AXP2101_I2C_ADDR), TAG, "AXP2101 not found on i2c bus");

    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_ADC_ENABLE, AXP2101_ADC_ENABLE_ALL),
                        TAG,
                        "enable ADCs failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_POWERKEY, AXP2101_POWERKEY_HOLD1S_OFF4S),
                        TAG,
                        "configure power key failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_PMU_CONFIG, AXP2101_PMU_CONFIG_DEFAULT),
                        TAG,
                        "pmu config failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_CHGLED, AXP2101_CHGLED_DEFAULT),
                        TAG,
                        "chgled config failed");

    /* Set every LDO's voltage before enabling any of them, matching the
     * reference bring-up sequence, so nothing glitches through a
     * default voltage on power-up. */
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_ALDO1_VOLTAGE, AXP2101_ALDO1_1V8),
                        TAG,
                        "set ALDO1 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_ALDO2_VOLTAGE, AXP2101_LDO_3V3),
                        TAG,
                        "set ALDO2 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_ALDO3_VOLTAGE, AXP2101_LDO_3V3),
                        TAG,
                        "set ALDO3 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_ALDO4_VOLTAGE, AXP2101_LDO_3V3),
                        TAG,
                        "set ALDO4 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_BLDO1_VOLTAGE, AXP2101_LDO_3V3),
                        TAG,
                        "set BLDO1 voltage failed");
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_DLDO1_VOLTAGE, AXP2101_LDO_3V3),
                        TAG,
                        "set DLDO1 voltage failed");

    /* Enable ALDO1-4 (digital core, system 3.3V, speaker amp, touch
     * power) and BLDO1 (LCD digital VDD). DLDO1 (backlight boost)
     * deliberately stays off here -- the SY7088 boost converter's
     * inrush current can brown out a near-flat battery at power-on, so
     * it's only enabled later via pmic_axp2101_set_backlight(), once
     * the rest of the system has settled. */
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_POWER_OUTPUT_CTRL,
                                      AXP2101_POWER_ALDO1_BIT | AXP2101_POWER_ALDO2_BIT |
                                          AXP2101_POWER_ALDO3_BIT | AXP2101_POWER_ALDO4_BIT |
                                          AXP2101_POWER_UNKNOWN_BIT4 | AXP2101_POWER_BLDO1_BIT),
                        TAG,
                        "enable LDOs failed");

    ESP_LOGI(TAG, "CoreS3 power rails enabled");
    return ESP_OK;
}

esp_err_t pmic_axp2101_set_backlight(uint8_t percent)
{
    uint8_t power_ctrl = 0;
    ESP_RETURN_ON_ERROR(i2c_bus_read_reg(AXP2101_I2C_ADDR, AXP2101_REG_POWER_OUTPUT_CTRL, &power_ctrl, 1),
                        TAG,
                        "read power output ctrl failed");

    if (percent == 0) {
        power_ctrl &= (uint8_t)~AXP2101_POWER_DLDO1_BIT;
        return axp2101_write(AXP2101_REG_POWER_OUTPUT_CTRL, power_ctrl);
    }

    if (percent > 100) {
        percent = 100;
    }
    const uint8_t scaled = (uint8_t)(((uint16_t)percent * 255U) / 100U);
    const uint8_t code = (uint8_t)(((uint16_t)scaled + AXP2101_DLDO1_CODE_OFFSET) >> AXP2101_DLDO1_CODE_SHIFT);
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_REG_DLDO1_VOLTAGE, code), TAG, "set backlight voltage failed");

    power_ctrl |= AXP2101_POWER_DLDO1_BIT;
    return axp2101_write(AXP2101_REG_POWER_OUTPUT_CTRL, power_ctrl);
}
