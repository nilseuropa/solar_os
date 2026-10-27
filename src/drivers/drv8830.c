#include "drv8830.h"

#define CONTROL 0x00U
#define FAULT 0x01U
#define CLEAR 0x80U

static esp_err_t write_byte(solar_os_drv8830_chip_t *chip, uint8_t reg, uint8_t value)
{
    return chip->io.write(chip->io.ctx, reg, &value, 1U);
}

esp_err_t solar_os_drv8830_chip_init(solar_os_drv8830_chip_t *chip,
                                     const solar_os_drv8830_io_t *io)
{
    if (chip == NULL || io == NULL || io->read == NULL || io->write == NULL) return ESP_ERR_INVALID_ARG;
    *chip = (solar_os_drv8830_chip_t){.io = *io};
    const esp_err_t ret = write_byte(chip, CONTROL, 0U);
    if (ret != ESP_OK) (void)write_byte(chip, CONTROL, 0U);
    chip->initialized = ret == ESP_OK;
    return ret;
}

esp_err_t solar_os_drv8830_chip_status(solar_os_drv8830_chip_t *chip,
                                       solar_os_drv8830_chip_status_t *status)
{
    if (chip == NULL || !chip->initialized) return ESP_ERR_INVALID_STATE;
    if (status == NULL) return ESP_ERR_INVALID_ARG;
    solar_os_drv8830_chip_status_t value = {0};
    esp_err_t ret = chip->io.read(chip->io.ctx, CONTROL, &value.control, 1U);
    if (ret == ESP_OK) ret = chip->io.read(chip->io.ctx, FAULT, &value.fault, 1U);
    if (ret != ESP_OK) return ret;
    value.mode = (solar_os_drv8830_mode_t)(value.control & 3U);
    const unsigned code = value.control >> 2U;
    /* TI Rev. G Table 1: code 6 = 0.48 V, code 32 = 2.57 V,
     * code 63 = 5.06 V. Codes 0..5 are reserved for voltage regulation. */
    if (code >= 6U) value.voltage_mv = (uint16_t)((code * 5140U + 32U) / 64U);
    *status = value;
    return ESP_OK;
}

esp_err_t solar_os_drv8830_chip_stop(solar_os_drv8830_chip_t *chip, bool brake)
{
    if (chip == NULL || !chip->initialized) return ESP_ERR_INVALID_STATE;
    /* Use a valid DAC code even though braking does not regulate voltage. */
    const esp_err_t ret = write_byte(chip, CONTROL, brake ? (6U << 2U) | 3U : 0U);
    if (ret != ESP_OK) (void)write_byte(chip, CONTROL, 0U);
    return ret;
}

esp_err_t solar_os_drv8830_chip_drive(solar_os_drv8830_chip_t *chip, int voltage_mv)
{
    if (chip == NULL || !chip->initialized) return ESP_ERR_INVALID_STATE;
    if (voltage_mv == 0) return solar_os_drv8830_chip_stop(chip, false);
    /* Check before negation, including INT_MIN. */
    if (voltage_mv < -5060 || voltage_mv > 5060 || (voltage_mv > -480 && voltage_mv < 480)) return ESP_ERR_INVALID_ARG;
    const unsigned magnitude = (unsigned)(voltage_mv < 0 ? -voltage_mv : voltage_mv);
    const unsigned code = (magnitude * 64U + 2570U) / 5140U;
    solar_os_drv8830_chip_status_t status;
    esp_err_t ret = solar_os_drv8830_chip_status(chip, &status);
    if (ret != ESP_OK) return ret;
    if (status.fault & SOLAR_OS_DRV8830_DRIVE_FAULT_MASK) return ESP_ERR_INVALID_STATE;
    const solar_os_drv8830_mode_t mode = voltage_mv < 0 ? SOLAR_OS_DRV8830_REVERSE : SOLAR_OS_DRV8830_FORWARD;
    /* Pass through standby on reversal, restarting the chip's voltage ramp. */
    if ((status.mode == SOLAR_OS_DRV8830_FORWARD || status.mode == SOLAR_OS_DRV8830_REVERSE) && status.mode != mode) {
        ret = solar_os_drv8830_chip_stop(chip, false);
        if (ret != ESP_OK) return ret;
    }
    ret = write_byte(chip, CONTROL, (uint8_t)((code << 2U) | mode));
    /* Never restore a previous moving command after an uncertain write. */
    if (ret != ESP_OK) (void)write_byte(chip, CONTROL, 0U);
    return ret;
}

esp_err_t solar_os_drv8830_chip_clear(solar_os_drv8830_chip_t *chip)
{
    esp_err_t ret = solar_os_drv8830_chip_stop(chip, false);
    if (ret == ESP_OK) ret = write_byte(chip, FAULT, CLEAR);
    return ret;
}
