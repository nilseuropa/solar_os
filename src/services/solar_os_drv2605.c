#include "solar_os_drv2605.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "drv2605.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_haptic.h"

#define DRV2605_DEVICE_MAX 2U
#define DRV2605_ADDRESS 0x5AU

typedef struct {
    bool active;
    bool power_control;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    solar_os_gpio_line_ref_t power_line;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    drv2605_t chip;
} drv2605_device_t;

static const char *TAG = "drv2605";
static EXT_RAM_BSS_ATTR drv2605_device_t devices[DRV2605_DEVICE_MAX];

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    drv2605_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus,
                                     device->address,
                                     reg,
                                     data,
                                     len);
}

static esp_err_t chip_write(void *ctx,
                            uint8_t reg,
                            const uint8_t *data,
                            size_t len)
{
    drv2605_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus,
                                      device->address,
                                      reg,
                                      data,
                                      len);
}

static void chip_delay_ms(void *ctx, uint32_t delay_ms)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
}

static esp_err_t play_effect(void *ctx, uint16_t effect)
{
    drv2605_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = drv2605_play_effect(&device->chip, effect);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t stop(void *ctx)
{
    drv2605_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = drv2605_stop(&device->chip);
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_haptic_ops_t haptic_ops = {
    .play_effect = play_effect,
    .stop = stop,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                bool *power_control,
                                solar_os_gpio_line_ref_t *power_line)
{
    bool have_i2c = false;
    bool have_address = false;
    if (bindings == NULL || i2c_bus == NULL || address == NULL ||
        power_control == NULL || power_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *power_control = false;
    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address && binding->value == DRV2605_ADDRESS) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE &&
                   strcmp(binding->role, "power") == 0 && !*power_control) {
            strlcpy(power_line->controller,
                    binding->target,
                    sizeof(power_line->controller));
            power_line->line = (uint8_t)binding->value;
            *power_control = true;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
        solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL) ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static void clear_device(drv2605_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->mutex != NULL) {
        vSemaphoreDelete(device->mutex);
    }
    memset(device, 0, sizeof(*device));
}

esp_err_t solar_os_drv2605_attach(
    const char *name,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    drv2605_device_t *device = NULL;
    for (size_t i = 0; i < DRV2605_DEVICE_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!devices[i].active && device == NULL) {
            device = &devices[i];
        }
    }
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    bool power_control = false;
    solar_os_gpio_line_ref_t power_line = {0};
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address,
                                       &power_control,
                                       &power_line),
                        TAG,
                        "invalid bindings");

    memset(device, 0, sizeof(*device));
    device->active = true;
    device->power_control = power_control;
    device->power_line = power_line;
    device->address = address;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        clear_device(device);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = ESP_OK;
    if (power_control) {
        ret = solar_os_gpio_line_write(&device->power_line, true);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2U));
        }
    }
    if (ret == ESP_OK) {
        ret = solar_os_bus_i2c_probe(i2c_bus, address);
    }
    const drv2605_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .delay_ms = chip_delay_ms,
        .ctx = device,
    };
    if (ret == ESP_OK) {
        ret = drv2605_init(&device->chip, &io);
    }
    if (ret != ESP_OK) {
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        clear_device(device);
        return ret;
    }

    const solar_os_haptic_registration_t registration = {
        .name = name,
        .driver = "drv2605",
        .effect_count = DRV2605_EFFECT_COUNT,
        .ops = &haptic_ops,
        .ctx = device,
    };
    ret = solar_os_haptic_register(&registration);
    if (ret != ESP_OK) {
        (void)drv2605_deinit(&device->chip);
        if (power_control) {
            (void)solar_os_gpio_line_write(&device->power_line, false);
        }
        clear_device(device);
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s addr=0x%02x power=%s chip-id=%u",
             name,
             i2c_bus,
             address,
             power_control ? "controlled" : "always-on",
             device->chip.chip_id);
    return ESP_OK;
}

esp_err_t solar_os_drv2605_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < DRV2605_DEVICE_MAX; i++) {
        drv2605_device_t *device = &devices[i];
        if (!device->active || strcmp(device->name, name) != 0) {
            continue;
        }
        ESP_RETURN_ON_ERROR(solar_os_haptic_unregister(name),
                            TAG,
                            "haptic device is busy");
        esp_err_t ret = drv2605_deinit(&device->chip);
        if (device->power_control) {
            const esp_err_t power_ret =
                solar_os_gpio_line_write(&device->power_line, false);
            if (ret == ESP_OK) {
                ret = power_ret;
            }
        }
        clear_device(device);
        return ret;
    }
    return ESP_ERR_NOT_FOUND;
}
