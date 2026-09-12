#include "solar_os_drv2605.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_buses.h"

/*
 * TI DRV2605 haptic driver as fitted to the LilyGO T-LoRa-Pager.
 * Operates in LRA (ERM/LRA auto-detect) mode using the internal ROM library.
 * Enable line is XL9555 GPIO0 (DRV_EN) — driven HIGH by tlora-pager-core at
 * boot, so the chip is already powered when this driver attaches.
 *
 * Register map from TI DRV2605 datasheet (SLOS854).
 */

#define DRV2605_REG_STATUS       0x00U
#define DRV2605_REG_MODE         0x01U
#define DRV2605_REG_RTP          0x02U
#define DRV2605_REG_LIBRARY      0x03U
#define DRV2605_REG_WAVESEQ1     0x04U  /* slots 1-8: 0x04-0x0B */
#define DRV2605_REG_GO           0x0CU
#define DRV2605_REG_FEEDBACK     0x1AU
#define DRV2605_REG_CONTROL3     0x1DU

#define DRV2605_MODE_INTERNAL_TRIGGER 0x00U
#define DRV2605_MODE_STANDBY          0x40U
#define DRV2605_LIBRARY_LRA           0x06U  /* LRA library */
#define DRV2605_FEEDBACK_LRA_BIT      0x80U  /* bit7: LRA mode */
#define DRV2605_GO                    0x01U
#define DRV2605_STOP                  0x00U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
} solar_os_drv2605_device_t;

static const char *TAG = "drv2605";
static solar_os_drv2605_device_t haptic_device;

static esp_err_t write_reg(uint8_t reg, uint8_t value)
{
    return solar_os_bus_i2c_write_reg(haptic_device.i2c_bus,
                                      haptic_device.address,
                                      reg, &value, 1);
}

static esp_err_t drv2605_init(void)
{
    /* Exit standby, set internal trigger mode */
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_MODE, DRV2605_MODE_INTERNAL_TRIGGER),
                        TAG, "mode write failed");
    /* Select LRA library */
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_LIBRARY, DRV2605_LIBRARY_LRA),
                        TAG, "library write failed");
    /* Configure feedback control for LRA */
    uint8_t fb = 0;
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_read_reg(haptic_device.i2c_bus, haptic_device.address,
                                  DRV2605_REG_FEEDBACK, &fb, 1),
        TAG, "feedback read failed");
    fb |= DRV2605_FEEDBACK_LRA_BIT;
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_FEEDBACK, fb), TAG, "feedback write failed");
    return ESP_OK;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address)
{
    bool have_i2c = false;
    bool have_address = false;

    if (bindings == NULL || i2c_bus == NULL || address == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_bus[0] = '\0';
    *address = 0U;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
            if (have_i2c) return ESP_ERR_INVALID_ARG;
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != SOLAR_OS_DRV2605_ADDRESS) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_drv2605_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;

    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (haptic_device.active) return ESP_ERR_INVALID_STATE;

    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus), &address),
                        TAG, "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address), TAG, "DRV2605 not found");

    memset(&haptic_device, 0, sizeof(haptic_device));
    haptic_device.address = address;
    strlcpy(haptic_device.name, name, sizeof(haptic_device.name));
    strlcpy(haptic_device.i2c_bus, i2c_bus, sizeof(haptic_device.i2c_bus));

    ESP_RETURN_ON_ERROR(drv2605_init(), TAG, "init failed");

    haptic_device.active = true;
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_drv2605_detach(const char *name)
{
    if (!haptic_device.active || name == NULL || strcmp(haptic_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    /* Put chip into standby */
    write_reg(DRV2605_REG_MODE, DRV2605_MODE_STANDBY);
    memset(&haptic_device, 0, sizeof(haptic_device));
    return ESP_OK;
}

esp_err_t solar_os_drv2605_play_effect(uint8_t effect_id)
{
    if (!haptic_device.active) return ESP_ERR_INVALID_STATE;
    if (effect_id == 0 || effect_id > 123) return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_WAVESEQ1, effect_id), TAG, "seq write failed");
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_WAVESEQ1 + 1, 0x00U), TAG, "seq term failed");
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_GO, DRV2605_GO), TAG, "go write failed");
    return ESP_OK;
}

esp_err_t solar_os_drv2605_play_sequence(const uint8_t *effects, size_t count)
{
    if (!haptic_device.active) return ESP_ERR_INVALID_STATE;
    if (effects == NULL || count == 0 || count > SOLAR_OS_DRV2605_WAVEFORM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; i++) {
        ESP_RETURN_ON_ERROR(
            write_reg((uint8_t)(DRV2605_REG_WAVESEQ1 + i), effects[i]),
            TAG, "seq slot write failed");
    }
    if (count < SOLAR_OS_DRV2605_WAVEFORM_MAX) {
        ESP_RETURN_ON_ERROR(
            write_reg((uint8_t)(DRV2605_REG_WAVESEQ1 + count), 0x00U),
            TAG, "seq term failed");
    }
    ESP_RETURN_ON_ERROR(write_reg(DRV2605_REG_GO, DRV2605_GO), TAG, "go write failed");
    return ESP_OK;
}

esp_err_t solar_os_drv2605_stop(void)
{
    if (!haptic_device.active) return ESP_ERR_INVALID_STATE;
    return write_reg(DRV2605_REG_GO, DRV2605_STOP);
}

bool solar_os_drv2605_is_active(void)
{
    return haptic_device.active;
}
