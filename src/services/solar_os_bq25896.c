#include "solar_os_bq25896.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bq25896.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_charger.h"

#define BQ25896_DEVICE_MAX 2U
#define BQ25896_ADDRESS 0x6BU

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    bq25896_t chip;
} bq25896_device_t;

typedef struct {
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    bool have_charge_current;
    uint16_t charge_current_ma;
    bool have_charge_voltage;
    uint16_t charge_voltage_mv;
} bq25896_bindings_t;

static const char *TAG = "bq25896";
static EXT_RAM_BSS_ATTR bq25896_device_t devices[BQ25896_DEVICE_MAX];

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    bq25896_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    bq25896_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t with_status(void *ctx, solar_os_charger_status_t *status)
{
    bq25896_device_t *device = ctx;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    bq25896_status_t chip_status;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = bq25896_read_status(&device->chip, &chip_status);
    xSemaphoreGive(device->mutex);
    if (ret == ESP_OK) {
        *status = (solar_os_charger_status_t) {
            .enabled = chip_status.enabled,
            .input_present = chip_status.input_present,
            .power_good = chip_status.power_good,
            .state = (solar_os_charger_state_t)(chip_status.state + 1U),
            .input_current_limit_ma = chip_status.input_current_limit_ma,
            .charge_current_ma = chip_status.charge_current_ma,
            .charge_voltage_mv = chip_status.charge_voltage_mv,
            .fault = chip_status.fault,
        };
    }
    return ret;
}

#define CHIP_SETTER(name, function) \
    static esp_err_t name(void *ctx, uint16_t value) \
    { \
        bq25896_device_t *device = ctx; \
        if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE; \
        xSemaphoreTake(device->mutex, portMAX_DELAY); \
        const esp_err_t ret = function(&device->chip, value); \
        xSemaphoreGive(device->mutex); \
        return ret; \
    }

CHIP_SETTER(set_input_current_limit, bq25896_set_input_current_limit)
CHIP_SETTER(set_charge_current, bq25896_set_charge_current)
CHIP_SETTER(set_charge_voltage, bq25896_set_charge_voltage)
#undef CHIP_SETTER

static esp_err_t set_enabled(void *ctx, bool enabled)
{
    bq25896_device_t *device = ctx;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = bq25896_set_enabled(&device->chip, enabled);
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_charger_ops_t charger_ops = {
    .read_status = with_status,
    .set_enabled = set_enabled,
    .set_input_current_limit = set_input_current_limit,
    .set_charge_current = set_charge_current,
    .set_charge_voltage = set_charge_voltage,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                bq25896_bindings_t *parsed)
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
                   !have_address && binding->value == BQ25896_ADDRESS) {
            parsed->address = (uint8_t)binding->value;
            have_address = true;
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

static void clear_device(bq25896_device_t *device)
{
    if (device->mutex != NULL) vSemaphoreDelete(device->mutex);
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_bq25896_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') return ESP_ERR_INVALID_ARG;
    bq25896_device_t *device = NULL;
    for (size_t i = 0; i < BQ25896_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0)
            return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    bq25896_bindings_t parsed;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &parsed),
                        TAG, "invalid bindings");
    memset(device, 0, sizeof(*device));
    device->active = true;
    device->address = parsed.address;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, parsed.i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = solar_os_bus_i2c_probe(device->i2c_bus, device->address);
    const bq25896_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
    };
    if (ret == ESP_OK) ret = bq25896_init(&device->chip, &io);
    if (ret == ESP_OK && parsed.have_charge_current)
        ret = bq25896_set_charge_current(&device->chip,
                                         parsed.charge_current_ma);
    if (ret == ESP_OK && parsed.have_charge_voltage)
        ret = bq25896_set_charge_voltage(&device->chip,
                                         parsed.charge_voltage_mv);
    if (ret != ESP_OK) {
        if (device->chip.initialized) (void)bq25896_deinit(&device->chip);
        clear_device(device);
        return ret;
    }

    const solar_os_charger_registration_t registration = {
        .name = name,
        .driver = "bq25896",
        .input_current_limit_ma = {
            BQ25896_INPUT_CURRENT_MIN_MA,
            BQ25896_INPUT_CURRENT_MAX_MA,
            BQ25896_INPUT_CURRENT_STEP_MA,
        },
        .charge_current_ma = {
            BQ25896_CHARGE_CURRENT_MIN_MA,
            BQ25896_CHARGE_CURRENT_MAX_MA,
            BQ25896_CHARGE_CURRENT_STEP_MA,
        },
        .charge_voltage_mv = {
            BQ25896_CHARGE_VOLTAGE_MIN_MV,
            BQ25896_CHARGE_VOLTAGE_MAX_MV,
            BQ25896_CHARGE_VOLTAGE_STEP_MV,
        },
        .ops = &charger_ops,
        .ctx = device,
    };
    ret = solar_os_charger_register(&registration);
    if (ret != ESP_OK) {
        (void)bq25896_deinit(&device->chip);
        clear_device(device);
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s addr=0x%02x part=%u rev=%u",
             name, device->i2c_bus, device->address,
             device->chip.part_number, device->chip.revision);
    return ESP_OK;
}

esp_err_t solar_os_bq25896_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < BQ25896_DEVICE_MAX; i++) {
        bq25896_device_t *device = &devices[i];
        if (!device->active || strcmp(device->name, name) != 0) continue;
        ESP_RETURN_ON_ERROR(solar_os_charger_unregister(name),
                            TAG, "charger device is busy");
        const esp_err_t ret = bq25896_deinit(&device->chip);
        clear_device(device);
        return ret;
    }
    return ESP_ERR_NOT_FOUND;
}
