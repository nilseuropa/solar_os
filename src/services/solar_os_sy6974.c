#include "solar_os_sy6974.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sy6974.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_charger.h"
#include "solar_os_battery.h"
#include "adc_port.h"
#include "driver/gpio.h"
#include "freertos/task.h"

#define SY6974_DEVICE_MAX 2U
#define SY6974_ADDRESS 0x6BU

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    sy6974_t chip;
    int adc_pin, power_pin, divider;
    bool battery_registered;
} sy6974_device_t;

typedef struct {
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    bool have_input_current;
    uint16_t input_current_ma;
    uint16_t input_voltage_mv;
    bool have_charge_current;
    uint16_t charge_current_ma;
    bool have_charge_voltage;
    uint16_t charge_voltage_mv;
    int adc_pin, power_pin, divider;
} sy6974_bindings_t;

static const char *TAG = "sy6974";
static EXT_RAM_BSS_ATTR sy6974_device_t devices[SY6974_DEVICE_MAX];

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    sy6974_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address,
                                     reg, data, len);
}

static esp_err_t chip_write(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    sy6974_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address,
                                      reg, data, len);
}

static esp_err_t with_status(void *ctx, solar_os_charger_status_t *status)
{
    sy6974_device_t *device = ctx;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    sy6974_status_t chip_status;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = device->active ? sy6974_read_status(&device->chip, &chip_status) :
        ESP_ERR_INVALID_STATE;
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
        sy6974_device_t *device = ctx; \
        if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE; \
        xSemaphoreTake(device->mutex, portMAX_DELAY); \
        const esp_err_t ret = device->active ? function(&device->chip, value) : ESP_ERR_INVALID_STATE; \
        xSemaphoreGive(device->mutex); \
        return ret; \
    }

CHIP_SETTER(set_input_current_limit, sy6974_set_input_current_limit)
CHIP_SETTER(set_charge_current, sy6974_set_charge_current)
CHIP_SETTER(set_charge_voltage, sy6974_set_charge_voltage)
#undef CHIP_SETTER

static esp_err_t set_enabled(void *ctx, bool enabled)
{
    sy6974_device_t *device = ctx;
    if (device == NULL || !device->active) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = device->active ? sy6974_set_enabled(&device->chip, enabled) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t battery_read(void *ctx, solar_os_battery_sample_t *sample)
{
    sy6974_device_t *device = ctx;
    if (!device || !device->active || device->adc_pin < 0 || !sample) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    if (!device->active) {
        xSemaphoreGive(device->mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (device->power_pin >= 0) {
        err = gpio_set_level(device->power_pin, 1);
        if (err == ESP_OK) {
            const TickType_t ticks = pdMS_TO_TICKS(10);
            vTaskDelay(ticks ? ticks : 1);
        }
    }
    adc_port_sample_t adc;
    sy6974_status_t status;
    if (err == ESP_OK) err = adc_port_read((gpio_num_t)device->adc_pin, &adc);
    if (err == ESP_OK) err = sy6974_read_status(&device->chip, &status);
    if (device->power_pin >= 0) {
        const esp_err_t off_err = gpio_set_level(device->power_pin, 0);
        if (err == ESP_OK) err = off_err;
    }
    if (err == ESP_OK && !adc.calibrated) err = ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) {
        const uint32_t mv = ((uint32_t)adc.voltage_mv * (unsigned)device->divider + 500U) / 1000U;
        *sample = (solar_os_battery_sample_t){.battery_mv = mv > UINT16_MAX ? UINT16_MAX : (uint16_t)mv,
            .calibrated = true, .external_power_valid = true, .external_power = status.input_present,
            .charging_valid = true, .charging = status.enabled && status.input_present &&
                (status.state == SY6974_CHARGE_STATE_PRECHARGE || status.state == SY6974_CHARGE_STATE_FAST)};
    }
    xSemaphoreGive(device->mutex);
    return err;
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
                                sy6974_bindings_t *parsed)
{
    bool have_i2c = false;
    bool have_address = false;
    if (bindings == NULL || parsed == NULL) return ESP_ERR_INVALID_ARG;
    memset(parsed, 0, sizeof(*parsed));
    parsed->adc_pin = parsed->power_pin = parsed->divider = -1;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(parsed->i2c_bus, binding->target, sizeof(parsed->i2c_bus));
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && binding->value == SY6974_ADDRESS) {
            parsed->address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "input_current") == 0 &&
                   !parsed->have_input_current && binding->value >= 100 &&
                   binding->value <= 3200 && (binding->value - 100) % 100 == 0) {
            parsed->input_current_ma = (uint16_t)binding->value;
            parsed->have_input_current = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "input_voltage") == 0 && !parsed->input_voltage_mv &&
                   binding->value >= 3900 && binding->value <= 5400 && (binding->value - 3900) % 100 == 0) {
            parsed->input_voltage_mv = (uint16_t)binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "charge_current") == 0 &&
                   !parsed->have_charge_current && binding->value >= 0 &&
                   binding->value <= 3000 && binding->value % 60 == 0) {
            parsed->charge_current_ma = (uint16_t)binding->value;
            parsed->have_charge_current = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "charge_voltage") == 0 &&
                   !parsed->have_charge_voltage && binding->value >= 3856 &&
                   binding->value <= 4624 && (binding->value - 3856) % 32 == 0) {
            parsed->charge_voltage_mv = (uint16_t)binding->value;
            parsed->have_charge_voltage = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_ADC && parsed->adc_pin < 0 &&
                   binding->value >= 0) {
            parsed->adc_pin = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   !strcmp(binding->role, "power") && parsed->power_pin < 0 &&
                   GPIO_IS_VALID_OUTPUT_GPIO(binding->value)) {
            parsed->power_pin = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   !strcmp(binding->role, "divider") && parsed->divider < 0 &&
                   binding->value >= 1000 && binding->value <= 10000) {
            parsed->divider = binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        ((parsed->adc_pin >= 0) == (parsed->divider >= 0)) &&
        (parsed->power_pin < 0 || (parsed->adc_pin >= 0 && parsed->adc_pin != parsed->power_pin)) &&
        solar_os_expansion_find_i2c_bus(parsed->i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(sy6974_device_t *device)
{
    if (device->mutex) xSemaphoreTake(device->mutex, portMAX_DELAY);
    device->active = false;
    if (device->power_pin >= 0 && device->adc_pin >= 0)
        (void)gpio_set_level(device->power_pin, 0);
    if (device->chip.initialized) (void)sy6974_deinit(&device->chip);
    device->name[0] = device->i2c_bus[0] = '\0';
    device->adc_pin = device->power_pin = device->divider = -1;
    device->battery_registered = false;
    /* Keep static mutex storage valid for a battery callback snapshotted before
     * unregistration. It rechecks active after acquiring this same mutex. */
    if (device->mutex) xSemaphoreGive(device->mutex);
}

esp_err_t solar_os_sy6974_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= SOLAR_OS_CHARGER_NAME_MAX) return ESP_ERR_INVALID_ARG;
    sy6974_device_t *device = NULL;
    for (size_t i = 0; i < SY6974_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0)
            return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && device == NULL) device = &devices[i];
    }
    if (device == NULL) return ESP_ERR_NO_MEM;

    sy6974_bindings_t parsed;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &parsed),
                        TAG, "invalid bindings");
    device->address = parsed.address;
    device->adc_pin = parsed.adc_pin;
    device->power_pin = parsed.power_pin;
    device->divider = parsed.divider;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, parsed.i2c_bus, sizeof(device->i2c_bus));
    if (!device->mutex) device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (device->adc_pin >= 0) ret = adc_port_configure_pin((gpio_num_t)device->adc_pin,
        ADC_ATTEN_DB_12, ADC_BITWIDTH_12);
    if (ret == ESP_OK && device->power_pin >= 0) {
        ret = gpio_set_level(device->power_pin, 0);
        if (ret == ESP_OK) ret = gpio_set_direction(device->power_pin, GPIO_MODE_OUTPUT);
    }
    if (ret == ESP_OK) ret = solar_os_bus_i2c_probe(device->i2c_bus, device->address);
    const sy6974_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
    };
    if (ret == ESP_OK) ret = sy6974_init(&device->chip, &io);
    if (ret == ESP_OK && parsed.have_input_current)
        ret = sy6974_set_input_current_limit(&device->chip, parsed.input_current_ma);
    if (ret == ESP_OK && parsed.input_voltage_mv)
        ret = sy6974_set_input_voltage_limit(&device->chip, parsed.input_voltage_mv);
    if (ret == ESP_OK && parsed.have_charge_current)
        ret = sy6974_set_charge_current(&device->chip,
                                         parsed.charge_current_ma);
    if (ret == ESP_OK && parsed.have_charge_voltage)
        ret = sy6974_set_charge_voltage(&device->chip,
                                         parsed.charge_voltage_mv);
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    const solar_os_charger_registration_t registration = {
        .name = name,
        .driver = "sy6974",
        .input_current_limit_ma = {
            SY6974_INPUT_CURRENT_MIN_MA,
            SY6974_INPUT_CURRENT_MAX_MA,
            SY6974_INPUT_CURRENT_STEP_MA,
        },
        .charge_current_ma = {
            SY6974_CHARGE_CURRENT_MIN_MA,
            SY6974_CHARGE_CURRENT_MAX_MA,
            SY6974_CHARGE_CURRENT_STEP_MA,
        },
        .charge_voltage_mv = {
            SY6974_CHARGE_VOLTAGE_MIN_MV,
            SY6974_CHARGE_VOLTAGE_MAX_MV,
            SY6974_CHARGE_VOLTAGE_STEP_MV,
        },
        .ops = &charger_ops,
        .ctx = device,
    };
    device->active = true;
    if (device->adc_pin >= 0) {
        const solar_os_battery_provider_t provider = {.read = battery_read, .user = device};
        ret = solar_os_battery_register_provider(name, &provider);
        if (ret != ESP_OK) {
            clear_device(device);
            return ret;
        }
        device->battery_registered = true;
    }
    ret = solar_os_charger_register(&registration);
    if (ret != ESP_OK) {
        if (device->battery_registered) (void)solar_os_battery_unregister_provider(name);
        clear_device(device);
        return ret;
    }
    ESP_LOGI(TAG, "%s attached on %s addr=0x%02x part=%u rev=%u",
             name, device->i2c_bus, device->address,
             device->chip.part_number, device->chip.revision);
    return ESP_OK;
}

esp_err_t solar_os_sy6974_detach(const char *name)
{
    if (name == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < SY6974_DEVICE_MAX; i++) {
        sy6974_device_t *device = &devices[i];
        if (!device->active || strcmp(device->name, name) != 0) continue;
        ESP_RETURN_ON_ERROR(solar_os_charger_unregister(name),
                            TAG, "charger device is busy");
        if (device->battery_registered) {
            const esp_err_t err = solar_os_battery_unregister_provider(name);
            if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) return err;
            device->battery_registered = false;
        }
        clear_device(device);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
