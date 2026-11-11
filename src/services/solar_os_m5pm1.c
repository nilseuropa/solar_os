#include "solar_os_m5pm1.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "m5pm1.h"
#include "solar_os_backlight.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_power.h"

void solar_os_ip2315_set_vin_present_fn(bool (*fn)(void)) __attribute__((weak));


#define M5PM1_DEVICE_MAX 1U
#define M5PM1_FRONTLIGHT_HZ 5000U
#define M5PM1_PWM_MAX 4095U

typedef struct {
    bool active;
    bool backlight_registered;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    int irq_pin;
    uint32_t capacity_mah;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_storage;
    m5pm1_t chip;
} solar_os_m5pm1_device_t;

static const char *TAG = "m5pm1";
static EXT_RAM_BSS_ATTR solar_os_m5pm1_device_t devices[M5PM1_DEVICE_MAX];

static solar_os_m5pm1_device_t *active_device(void)
{
    return devices[0].active ? &devices[0] : NULL;
}

static esp_err_t chip_read(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    solar_os_m5pm1_device_t *device = ctx;
    return solar_os_bus_i2c_read_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t chip_write(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    solar_os_m5pm1_device_t *device = ctx;
    return solar_os_bus_i2c_write_reg(device->i2c_bus, device->address, reg, data, len);
}

static esp_err_t controller_configure(void *ctx,
                                      uint8_t line,
                                      solar_os_gpio_line_mode_t mode,
                                      solar_os_gpio_line_pull_t pull)
{
    solar_os_m5pm1_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    if (pull != SOLAR_OS_GPIO_LINE_PULL_NONE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = m5pm1_configure(&device->chip,
                                          line,
                                          mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t controller_read(void *ctx, uint8_t line, bool *level)
{
    solar_os_m5pm1_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = m5pm1_read(&device->chip, line, level);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t controller_write(void *ctx, uint8_t line, bool level)
{
    solar_os_m5pm1_device_t *device = ctx;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = m5pm1_write(&device->chip, line, level);
    xSemaphoreGive(device->mutex);
    return ret;
}

static const solar_os_gpio_controller_ops_t controller_ops = {
    .configure = controller_configure,
    .read = controller_read,
    .write = controller_write,
};

static uint16_t brightness_to_duty(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    const uint32_t scaled = ((uint32_t)percent * (uint32_t)percent * M5PM1_PWM_MAX) / 10000U;
    return (uint16_t)scaled;
}

static esp_err_t set_backlight(void *user, uint8_t percent)
{
    solar_os_m5pm1_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t duty = brightness_to_duty(percent);
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = m5pm1_set_pwm0_duty(&device->chip, duty, percent != 0U);
    xSemaphoreGive(device->mutex);
    return ret;
}

static esp_err_t battery_read(void *user, solar_os_battery_sample_t *sample)
{
    solar_os_m5pm1_device_t *device = user;
    if (device == NULL || !device->active || sample == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(sample, 0, sizeof(*sample));
    uint16_t vbat = 0U;
    uint16_t vin = 0U;
    uint8_t source = 0U;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = m5pm1_read_vbat_mv(&device->chip, &vbat);
    if (ret == ESP_OK) {
        ret = m5pm1_read_vin_mv(&device->chip, &vin);
    }
    if (ret == ESP_OK) {
        ret = m5pm1_read_power_source(&device->chip, &source);
    }
    xSemaphoreGive(device->mutex);
    if (ret != ESP_OK) {
        return ret;
    }
    sample->battery_mv = vbat;
    sample->calibrated = vbat > 0U;
    sample->external_power = (source & (M5PM1_PWR_SRC_VIN | M5PM1_PWR_SRC_VINOUT)) != 0U ||
        vin > 4000U;
    sample->external_power_valid = true;
    return ESP_OK;
}

static esp_err_t power_off(void *user)
{
    solar_os_m5pm1_device_t *device = user;
    if (device == NULL || !device->active) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    const esp_err_t ret = m5pm1_shutdown(&device->chip);
    xSemaphoreGive(device->mutex);
    return ret;
}

esp_err_t solar_os_m5pm1_input_present(bool *present)
{
    if (present == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_m5pm1_device_t *device = active_device();
    if (device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t source = 0U;
    uint16_t vin = 0U;
    xSemaphoreTake(device->mutex, portMAX_DELAY);
    esp_err_t ret = m5pm1_read_power_source(&device->chip, &source);
    if (ret == ESP_OK) {
        ret = m5pm1_read_vin_mv(&device->chip, &vin);
    }
    xSemaphoreGive(device->mutex);
    if (ret == ESP_OK) {
        *present = (source & (M5PM1_PWR_SRC_VIN | M5PM1_PWR_SRC_VINOUT)) != 0U ||
            vin > 4000U;
    }
    return ret;
}


static bool ip2315_vin_present(void)
{
    bool present = false;
    return solar_os_m5pm1_input_present(&present) == ESP_OK && present;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *i2c_bus,
                                size_t i2c_bus_len,
                                uint8_t *address,
                                uint8_t *output,
                                uint8_t *direction,
                                int *irq_pin,
                                uint32_t *capacity_mah)
{
    bool have_i2c = false;
    bool have_address = false;
    *output = 0U;
    *direction = 0U;
    *irq_pin = -1;
    *capacity_mah = 0U;
    for (size_t i = 0; bindings != NULL && i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !have_i2c) {
            strlcpy(i2c_bus, binding->target, i2c_bus_len);
            have_i2c = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS &&
                   !have_address) {
            *address = (uint8_t)binding->value;
            have_address = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   strcmp(binding->role, "irq") == 0) {
            *irq_pin = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "output") == 0) {
            *output = (uint8_t)binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "direction") == 0) {
            *direction = (uint8_t)binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   strcmp(binding->role, "capacity") == 0) {
            *capacity_mah = (uint32_t)binding->value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_i2c && have_address &&
            solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)
        ? ESP_OK
        : ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_m5pm1_attach(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_m5pm1_device_t *device = &devices[0];
    if (device->active) {
        return ESP_ERR_INVALID_STATE;
    }

    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0U;
    uint8_t output = 0U;
    uint8_t direction = 0U;
    int irq_pin = -1;
    uint32_t capacity_mah = 0U;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, i2c_bus,
                                       sizeof(i2c_bus), &address, &output,
                                       &direction, &irq_pin, &capacity_mah),
                        TAG, "invalid bindings");
    ESP_RETURN_ON_ERROR(solar_os_bus_i2c_probe(i2c_bus, address),
                        TAG, "M5PM1 not found");

    memset(device, 0, sizeof(*device));
    device->address = address;
    device->irq_pin = irq_pin;
    device->capacity_mah = capacity_mah;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));
    device->mutex = xSemaphoreCreateMutexStatic(&device->mutex_storage);
    if (device->mutex == NULL) {
        memset(device, 0, sizeof(*device));
        return ESP_ERR_NO_MEM;
    }
    const m5pm1_io_t io = {
        .read = chip_read,
        .write = chip_write,
        .ctx = device,
    };
    esp_err_t ret = m5pm1_init(&device->chip, &io, output, direction);
    if (ret != ESP_OK) {
        memset(device, 0, sizeof(*device));
        return ret;
    }
    ret = m5pm1_configure_pwm0(&device->chip, M5PM1_FRONTLIGHT_HZ);
    if (ret != ESP_OK) {
        (void)m5pm1_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    device->active = true;
    const solar_os_gpio_controller_registration_t registration = {
        .name = name,
        .line_count = M5PM1_LINE_COUNT,
        .ops = &controller_ops,
        .ctx = device,
    };
    ret = solar_os_gpio_controller_register(&registration);
    if (ret != ESP_OK) {
        (void)m5pm1_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    const solar_os_battery_provider_t battery = {
        .read = battery_read,
        .user = device,
    };
    ret = solar_os_battery_register_provider(name, &battery);
    if (ret != ESP_OK) {
        (void)solar_os_gpio_controller_unregister(name);
        (void)m5pm1_deinit(&device->chip);
        memset(device, 0, sizeof(*device));
        return ret;
    }
    if (capacity_mah > 0U) {
        (void)solar_os_battery_set_capacity_mah(capacity_mah);
    }
    (void)solar_os_power_register_off_provider(name, power_off, device);
    ret = solar_os_backlight_register(set_backlight, device);
    if (ret == ESP_OK) {
        device->backlight_registered = true;
    } else {
        ESP_LOGW(TAG, "frontlight registration failed: %s", esp_err_to_name(ret));
    }
    if (solar_os_ip2315_set_vin_present_fn != NULL) {
        solar_os_ip2315_set_vin_present_fn(ip2315_vin_present);
    }
    ESP_LOGI(TAG, "%s attached on %s address 0x%02x", name, i2c_bus, address);
    return ESP_OK;
}

esp_err_t solar_os_m5pm1_detach(const char *name)
{
    if (name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_m5pm1_device_t *device = &devices[0];
    if (!device->active || strcmp(device->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (solar_os_ip2315_set_vin_present_fn != NULL) {
        solar_os_ip2315_set_vin_present_fn(NULL);
    }
    if (device->backlight_registered) {
        solar_os_backlight_unregister(device);
        device->backlight_registered = false;
    }
    solar_os_power_unregister_off_provider(device);
    (void)solar_os_battery_unregister_provider(name);
    ESP_RETURN_ON_ERROR(solar_os_gpio_controller_unregister(name),
                        TAG, "GPIO controller is busy");
    ESP_RETURN_ON_ERROR(m5pm1_deinit(&device->chip), TAG, "deinit failed");
    memset(device, 0, sizeof(*device));
    return ESP_OK;
}
