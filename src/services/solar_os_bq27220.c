#include "solar_os_bq27220.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"

/*
 * TI BQ27220 Impedance Track fuel gauge, as fitted to the LilyGO T-LoRa-Pager.
 * Standard-command register offsets and the BatteryStatus() bit layout below
 * are taken from LilyGoLib's own GaugeBQ27220 driver
 * (SensorLib/src/REG/BQ27220Constants.h, GaugeBQ27220.hpp), not the datasheet
 * directly, to match what LilyGO's shipped firmware actually reads.
 *
 * This board also has a BQ25896 charger IC on the same bus, which is not
 * wired up here. Charging/external-power state below is inferred from the
 * gauge's own BatteryStatus() DSG bit (set = discharging, clear = charging
 * or relaxing) rather than measured at the charger, so it cannot distinguish
 * "charging" from "full and idle on USB power".
 */

#define BQ27220_REG_VOLTAGE 0x08U /* mV, uint16 LE */
#define BQ27220_REG_BATTERY_STATUS 0x0AU /* bitmap, uint16 LE */
#define BQ27220_READ_LEN 4U /* covers REG_VOLTAGE and REG_BATTERY_STATUS in one transfer */

#define BQ27220_STATUS_DSG_BIT 0x0001U /* LSB bit0: set while discharging */

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
} solar_os_bq27220_device_t;

static const char *TAG = "bq27220";
static solar_os_bq27220_device_t gauge_device;

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
            if (have_i2c) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
            if (have_address || binding->value != SOLAR_OS_BQ27220_ADDRESS) {
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

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_bq27220_device_t *device = user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t data[BQ27220_READ_LEN] = {0};
    ESP_RETURN_ON_ERROR(
        solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, BQ27220_REG_VOLTAGE, data, sizeof(data)),
        TAG, "read failed");

    const uint16_t voltage_mv = (uint16_t)(data[0] | (data[1] << 8));
    const uint16_t status = (uint16_t)(data[2] | (data[3] << 8));
    const bool discharging = (status & BQ27220_STATUS_DSG_BIT) != 0U;

    *sample = (solar_os_battery_sample_t) {
        .battery_mv = voltage_mv,
        .calibrated = true,
        .external_power_valid = true,
        .external_power = !discharging,
        .charging_valid = true,
        .charging = !discharging,
    };
    return ESP_OK;
}

static void clear_device(solar_os_bq27220_device_t *device)
{
    if (device == NULL) {
        return;
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_bq27220_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;

    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (gauge_device.active) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus, sizeof(i2c_bus), &address),
                        TAG, "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address), TAG, "BQ27220 not found");

    clear_device(&gauge_device);
    gauge_device.active = true;
    gauge_device.address = address;
    strlcpy(gauge_device.name, name, sizeof(gauge_device.name));
    strlcpy(gauge_device.i2c_bus, i2c_bus, sizeof(gauge_device.i2c_bus));

    const solar_os_battery_provider_t provider = {
        .read = battery_read,
        .user = &gauge_device,
    };
    const esp_err_t ret = solar_os_battery_register_provider(name, &provider);
    if (ret != ESP_OK) {
        clear_device(&gauge_device);
        return ret;
    }

    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_bq27220_detach(const char *name)
{
    if (!gauge_device.active || name == NULL || strcmp(gauge_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(solar_os_battery_unregister_provider(name), TAG, "unregister failed");
    clear_device(&gauge_device);
    return ESP_OK;
}
