#include "solar_os_axp2101.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "axp2101.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_charger.h"
#include "solar_os_power.h"

#define AXP2101_CHARGER_NAME "charger0"

typedef struct {
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    bool have_input_current;
    uint16_t input_current_ma;
    bool have_charge_current;
    uint16_t charge_current_ma;
    bool have_charge_voltage;
    uint16_t charge_voltage_mv;
} axp2101_bindings_t;

typedef struct {
    bool active;
    size_t aldo3_users;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    axp2101_t chip;
} solar_os_axp2101_device_t;

static const char *TAG = "axp2101";
static solar_os_axp2101_device_t power_device;

static esp_err_t chip_read(void *context,
                           uint8_t reg,
                           uint8_t *data,
                           size_t len)
{
    solar_os_axp2101_device_t *device = context;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *context,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    solar_os_axp2101_device_t *device = context;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t power_off(void *context)
{
    solar_os_axp2101_device_t *device = context;
    if (!device->active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t err = axp2101_power_off(&device->chip);
    xSemaphoreGive(device->mutex);
    return err;
}

static esp_err_t battery_read(void *context,
                              solar_os_battery_sample_t *sample)
{
    solar_os_axp2101_device_t *device = context;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    axp2101_battery_status_t status;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = axp2101_read_battery(&device->chip, &status);
    xSemaphoreGive(device->mutex);
    if (ret == ESP_OK) {
        *sample = (solar_os_battery_sample_t) {
            .battery_mv = status.battery_mv,
            .calibrated = true,
            .percent_valid = status.percent_valid,
            .percent = status.percent,
            .external_power_valid = true,
            .external_power = status.external_power,
            .charging_valid = true,
            .charging = status.charging,
        };
    }
    return ret;
}

static solar_os_charger_state_t charger_state(axp2101_charge_state_t state)
{
    switch (state) {
    case AXP2101_CHARGE_STATE_TRICKLE:
    case AXP2101_CHARGE_STATE_PRECHARGE:
        return SOLAR_OS_CHARGER_STATE_PRECHARGE;
    case AXP2101_CHARGE_STATE_CONSTANT_CURRENT:
    case AXP2101_CHARGE_STATE_CONSTANT_VOLTAGE:
        return SOLAR_OS_CHARGER_STATE_FAST_CHARGE;
    case AXP2101_CHARGE_STATE_DONE:
        return SOLAR_OS_CHARGER_STATE_DONE;
    case AXP2101_CHARGE_STATE_STOPPED:
        return SOLAR_OS_CHARGER_STATE_NOT_CHARGING;
    default:
        return SOLAR_OS_CHARGER_STATE_UNKNOWN;
    }
}

static esp_err_t charger_read(void *context,
                              solar_os_charger_status_t *status)
{
    solar_os_axp2101_device_t *device = context;
    if (device == NULL || !device->active || status == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    axp2101_charger_status_t chip_status;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = axp2101_read_charger(&device->chip, &chip_status);
    xSemaphoreGive(device->mutex);
    if (ret == ESP_OK) {
        *status = (solar_os_charger_status_t) {
            .enabled = chip_status.enabled,
            .input_present = chip_status.input_present,
            .power_good = chip_status.power_good,
            .state = charger_state(chip_status.state),
            .input_current_limit_ma = chip_status.input_current_limit_ma,
            .charge_current_ma = chip_status.charge_current_ma,
            .charge_voltage_mv = chip_status.charge_voltage_mv,
            .fault = chip_status.fault,
        };
    }
    return ret;
}

static esp_err_t charger_set_enabled(void *context, bool enabled)
{
    solar_os_axp2101_device_t *device = context;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = axp2101_set_charger_enabled(&device->chip, enabled);
    xSemaphoreGive(device->mutex);
    return ret;
}

#define AXP2101_SETTER(name, function) \
    static esp_err_t name(void *context, uint16_t value) \
    { \
        solar_os_axp2101_device_t *device = context; \
        if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE; \
        xSemaphoreTake(device->mutex, portMAX_DELAY); \
        const esp_err_t ret = function(&device->chip, value); \
        xSemaphoreGive(device->mutex); \
        return ret; \
    }

AXP2101_SETTER(charger_set_input_current, axp2101_set_input_current_limit)
AXP2101_SETTER(charger_set_charge_current, axp2101_set_charge_current)
AXP2101_SETTER(charger_set_charge_voltage, axp2101_set_charge_voltage)
#undef AXP2101_SETTER

static const solar_os_charger_ops_t charger_ops = {
    .read_status = charger_read,
    .set_enabled = charger_set_enabled,
    .set_input_current_limit = charger_set_input_current,
    .set_charge_current = charger_set_charge_current,
    .set_charge_voltage = charger_set_charge_voltage,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                axp2101_bindings_t *parsed)
{
    bool have_i2c = false;
    bool have_address = false;
    if (bindings == NULL || parsed == NULL) return ESP_ERR_INVALID_ARG;
    memset(parsed, 0, sizeof(*parsed));
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(parsed->i2c_bus, binding->target, sizeof(parsed->i2c_bus));
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && binding->value == AXP2101_I2C_ADDRESS) {
            parsed->address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "input_current") == 0 &&
                   !parsed->have_input_current) {
            parsed->input_current_ma = (uint16_t)binding->value;
            parsed->have_input_current = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "charge_current") == 0 &&
                   !parsed->have_charge_current) {
            parsed->charge_current_ma = (uint16_t)binding->value;
            parsed->have_charge_current = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "charge_voltage") == 0 &&
                   !parsed->have_charge_voltage) {
            parsed->charge_voltage_mv = (uint16_t)binding->value;
            parsed->have_charge_voltage = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        solar_os_expansion_find_i2c_bus(parsed->i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(void)
{
    solar_os_power_unregister_off_provider(&power_device);
    if (power_device.mutex != NULL) {
        vSemaphoreDelete(power_device.mutex);
    }
    memset(&power_device, 0, sizeof(power_device));
}

esp_err_t solar_os_axp2101_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (power_device.active) return ESP_ERR_INVALID_STATE;
    axp2101_bindings_t parsed;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &parsed),
                        TAG, "invalid bindings");

    memset(&power_device, 0, sizeof(power_device));
    power_device.active = true;
    power_device.address = parsed.address;
    strlcpy(power_device.name, name, sizeof(power_device.name));
    strlcpy(power_device.i2c_bus, parsed.i2c_bus,
            sizeof(power_device.i2c_bus));
    power_device.mutex = xSemaphoreCreateMutexStatic(&power_device.mutex_storage);
    if (power_device.mutex == NULL) {
        clear_device();
        return ESP_ERR_NO_MEM;
    }

    const axp2101_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = &power_device,
    };
    esp_err_t ret = axp2101_init(&power_device.chip, &io);
    if (ret == ESP_OK) ret = axp2101_enable_monitoring(&power_device.chip);
    if (ret == ESP_OK && parsed.have_input_current) {
        ret = axp2101_set_input_current_limit(&power_device.chip,
                                               parsed.input_current_ma);
    }
    if (ret == ESP_OK && parsed.have_charge_current) {
        ret = axp2101_set_charge_current(&power_device.chip,
                                         parsed.charge_current_ma);
    }
    if (ret == ESP_OK && parsed.have_charge_voltage) {
        ret = axp2101_set_charge_voltage(&power_device.chip,
                                         parsed.charge_voltage_mv);
    }
    if (ret != ESP_OK) {
        if (power_device.chip.initialized) {
            (void)axp2101_deinit(&power_device.chip);
        }
        clear_device();
        return ret;
    }

    const solar_os_battery_provider_t battery_provider = {
        .read = battery_read,
        .user = &power_device,
    };
    ret = solar_os_battery_register_provider(name, &battery_provider);
    if (ret != ESP_OK) {
        (void)axp2101_deinit(&power_device.chip);
        clear_device();
        return ret;
    }

    const solar_os_charger_registration_t charger_registration = {
        .name = AXP2101_CHARGER_NAME,
        .driver = "axp2101",
        .input_current_limit_ma = {500U, 2000U, 500U},
        .charge_current_ma = {100U, 1000U, 100U},
        .charge_voltage_mv = {4000U, 4200U, 100U},
        .ops = &charger_ops,
        .ctx = &power_device,
    };
    ret = solar_os_charger_register(&charger_registration);
    if (ret != ESP_OK) {
        (void)solar_os_battery_unregister_provider(name);
        (void)axp2101_deinit(&power_device.chip);
        clear_device();
        return ret;
    }

    (void)solar_os_power_register_off_provider(name, power_off, &power_device);
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x; charger=%s",
             name, power_device.i2c_bus, power_device.address,
             AXP2101_CHARGER_NAME);
    return ESP_OK;
}

esp_err_t solar_os_axp2101_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    if (!power_device.active || strcmp(power_device.name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(power_device.mutex, portMAX_DELAY);
    const bool panel_active = power_device.aldo3_users > 0U;
    xSemaphoreGive(power_device.mutex);
    if (panel_active) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(solar_os_charger_unregister(AXP2101_CHARGER_NAME),
                        TAG, "charger is busy");
    const esp_err_t battery_ret = solar_os_battery_unregister_provider(name);
    if (battery_ret != ESP_OK) return battery_ret;
    const esp_err_t ret = axp2101_deinit(&power_device.chip);
    clear_device();
    return ret;
}

esp_err_t solar_os_axp2101_acquire_aldo3(const char *i2c_bus, uint8_t address)
{
    if (i2c_bus == NULL) return ESP_ERR_INVALID_ARG;
    if (!power_device.active || !power_device.chip.initialized ||
        power_device.address != address ||
        strcmp(power_device.i2c_bus, i2c_bus) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(power_device.mutex, portMAX_DELAY);
    /* One panel owns the rail; another panel must not switch it off. */
    const esp_err_t ret = power_device.aldo3_users == 0U ?
        ESP_OK : ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) power_device.aldo3_users++;
    xSemaphoreGive(power_device.mutex);
    return ret;
}

void solar_os_axp2101_release_aldo3(void)
{
    if (!power_device.active) return;
    xSemaphoreTake(power_device.mutex, portMAX_DELAY);
    if (power_device.aldo3_users > 0U) power_device.aldo3_users--;
    xSemaphoreGive(power_device.mutex);
}

esp_err_t solar_os_axp2101_set_aldo3(bool enabled)
{
    if (!power_device.active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(power_device.mutex, portMAX_DELAY);
    const esp_err_t ret = power_device.aldo3_users > 0U ?
        axp2101_set_aldo3(&power_device.chip, enabled, 3300U) :
        ESP_ERR_INVALID_STATE;
    xSemaphoreGive(power_device.mutex);
    return ret;
}
