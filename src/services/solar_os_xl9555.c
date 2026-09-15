#include "solar_os_xl9555.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "xl9555.h"

#define XL9555_DEVICE_MAX 2U

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    xl9555_t chip;
} solar_os_xl9555_device_t;

static const char *TAG = "xl9555";
static solar_os_xl9555_device_t devices[XL9555_DEVICE_MAX];

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    solar_os_xl9555_device_t *device = ctx;
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
    solar_os_xl9555_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus,
                                      device->address,
                                      reg,
                                      data,
                                      len);
}

static esp_err_t controller_configure(void *ctx,
                                      uint8_t line,
                                      solar_os_gpio_line_mode_t mode,
                                      solar_os_gpio_line_pull_t pull)
{
    solar_os_xl9555_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pull != SOLAR_OS_GPIO_LINE_PULL_NONE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = xl9555_configure(&device->chip,
                                           line,
                                           mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t controller_read(void *ctx, uint8_t line, bool *level)
{
    solar_os_xl9555_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = xl9555_read(&device->chip, line, level);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t controller_write(void *ctx, uint8_t line, bool level)
{
    solar_os_xl9555_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = xl9555_write(&device->chip, line, level);
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gpio_controller_ops_t controller_ops = {
    .configure = controller_configure,
    .read = controller_read,
    .write = controller_write,
};

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                uint16_t *output,
                                uint16_t *direction)
{
    bool have_i2c = false;
    bool have_address = false;
    *output = UINT16_MAX;
    *direction = UINT16_MAX;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "output") == 0) {
            *output = (uint16_t)binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "direction") == 0) {
            *direction = (uint16_t)binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_xl9555_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_xl9555_device_t *device = NULL;
    for (size_t i = 0; i < XL9555_DEVICE_MAX; i++) {
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
    uint16_t output = UINT16_MAX;
    uint16_t direction = UINT16_MAX;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address,
                                       &output,
                                       &direction),
                        TAG,
                        "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG,
                        "XL9555 not found");

    memset(device, 0, sizeof(*device));
    device->address = address;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    const xl9555_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
    };
    esp_err_t ret = xl9555_init(&device->chip, &io, output, direction);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->active = true;
    const solar_os_gpio_controller_registration_t registration = {
        .name = name,
        .line_count = XL9555_LINE_COUNT,
        .ops = &controller_ops,
        .ctx = device,
    };
    ret = solar_os_gpio_controller_register(&registration);
    if (ret != ESP_OK) {
        (void)xl9555_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x output=0x%04x direction=0x%04x",
             name,
             i2c_bus,
             address,
             output,
             direction);
    return ESP_OK;
}

esp_err_t solar_os_xl9555_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < XL9555_DEVICE_MAX; i++) {
        solar_os_xl9555_device_t *device = &devices[i];
        if (!device->active || strcmp(device->name, name) != 0) {
            continue;
        }
        ESP_RETURN_ON_ERROR(solar_os_gpio_controller_unregister(name),
                            TAG,
                            "GPIO controller is busy");
        ESP_RETURN_ON_ERROR(xl9555_deinit(&device->chip), TAG, "deinit failed");
        memset(device, 0, sizeof(*device));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
