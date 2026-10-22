#include "solar_os_waveshare_io.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_backlight.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "solar_os_gpio_controller.h"
#include "waveshare_io.h"

typedef struct {
    bool active, battery, backlight;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    unsigned vref, divider;
    solar_os_ws_io_chip_t chip;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t storage;
} ws_io_device_t;

/* Mutex storage must remain in internal memory. GPIO-controller callbacks and
 * battery/backlight callbacks share one lock around the firmware registers. */
static ws_io_device_t devices[2];

static esp_err_t read_reg(void *ctx, uint8_t reg, uint8_t *data, size_t len)
{
    ws_io_device_t *d = ctx;
    return solar_os_bus_i2c_read_reg(d->bus, d->address, reg, data, len);
}
static esp_err_t write_reg(void *ctx, uint8_t reg, const uint8_t *data, size_t len)
{
    ws_io_device_t *d = ctx;
    return solar_os_bus_i2c_write_reg(d->bus, d->address, reg, data, len);
}
static esp_err_t configure(void *ctx, uint8_t line,
    solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{
    ws_io_device_t *d = ctx;
    if (pull != SOLAR_OS_GPIO_LINE_PULL_NONE) return ESP_ERR_NOT_SUPPORTED;
    if (mode != SOLAR_OS_GPIO_LINE_MODE_INPUT && mode != SOLAR_OS_GPIO_LINE_MODE_OUTPUT)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    const esp_err_t err = solar_os_ws_io_chip_configure(&d->chip, line, mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT);
    xSemaphoreGive(d->mutex);
    return err;
}
static esp_err_t read_line(void *ctx, uint8_t line, bool *level)
{
    ws_io_device_t *d = ctx;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    const esp_err_t err = solar_os_ws_io_chip_read(&d->chip, line, level);
    xSemaphoreGive(d->mutex);
    return err;
}
static esp_err_t write_line(void *ctx, uint8_t line, bool level)
{
    ws_io_device_t *d = ctx;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    const esp_err_t err = solar_os_ws_io_chip_write(&d->chip, line, level);
    xSemaphoreGive(d->mutex);
    return err;
}
static const solar_os_gpio_controller_ops_t ops = {
    .configure = configure, .read = read_line, .write = write_line,
};

static esp_err_t battery_read(void *ctx, solar_os_battery_sample_t *sample)
{
    ws_io_device_t *d = ctx;
    uint16_t raw;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    const esp_err_t err = solar_os_ws_io_chip_adc(&d->chip, &raw);
    xSemaphoreGive(d->mutex);
    if (err != ESP_OK) return err;
    const uint64_t mv = ((uint64_t)raw * d->vref * d->divider + 511500U) / 1023000U;
    if (mv > UINT16_MAX) return ESP_ERR_INVALID_RESPONSE;
    /* Nominal reference/divider conversion; no calibration data is exposed. */
    *sample = (solar_os_battery_sample_t){.battery_mv = (uint16_t)mv, .calibrated = false};
    return ESP_OK;
}
static esp_err_t backlight_set(void *ctx, uint8_t percent)
{
    ws_io_device_t *d = ctx;
    if (percent > 100) return ESP_ERR_INVALID_ARG;
    unsigned duty = ((100U - percent) * 255U + 50U) / 100U;
    if (percent && duty > 247U) duty = 247U;
    xSemaphoreTake(d->mutex, portMAX_DELAY);
    const esp_err_t err = solar_os_ws_io_chip_pwm(&d->chip, (uint8_t)duty);
    xSemaphoreGive(d->mutex);
    return err;
}

esp_err_t solar_os_waveshare_io_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || strlen(name) >= SOLAR_OS_GPIO_CONTROLLER_NAME_MAX || !bindings)
        return ESP_ERR_INVALID_ARG;
    ws_io_device_t *d = NULL;
    for (size_t i = 0; i < 2; i++) {
        if (devices[i].active && strcmp(name, devices[i].name) == 0) return ESP_ERR_INVALID_STATE;
        if (!devices[i].active && !d) d = &devices[i];
    }
    if (!d) return ESP_ERR_NO_MEM;
    memset(d, 0, sizeof(*d));
    unsigned output = 255, direction = 0;
    d->vref = 3300; d->divider = 3000;
    bool bus = false, address = false;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_BUS && !bus) {
            strlcpy(d->bus, b->target, sizeof(d->bus)); bus = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS && !address && b->value == 0x24) {
            d->address = 0x24; address = true;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (!strcmp(b->role, "output") && b->value >= 0 && b->value <= 255) output = b->value;
            else if (!strcmp(b->role, "direction") && b->value >= 0 && b->value <= 255) direction = b->value;
            else if (!strcmp(b->role, "battery") && b->value >= 0 && b->value <= 1) d->battery = b->value;
            else if (!strcmp(b->role, "backlight") && b->value >= 0 && b->value <= 1) d->backlight = b->value;
            else if (!strcmp(b->role, "vref") && b->value >= 1000 && b->value <= 5000) d->vref = b->value;
            else if (!strcmp(b->role, "divider") && b->value >= 1000 && b->value <= 10000) d->divider = b->value;
            else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (!bus || !address || !solar_os_expansion_find_i2c_bus(d->bus, NULL, NULL)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = solar_os_bus_i2c_probe(d->bus, d->address);
    if (err != ESP_OK) return err;
    strlcpy(d->name, name, sizeof(d->name));
    d->mutex = xSemaphoreCreateMutexStatic(&d->storage);
    if (!d->mutex) return ESP_ERR_NO_MEM;
    const solar_os_ws_io_transport_t io = {.read = read_reg, .write = write_reg, .ctx = d};
    err = solar_os_ws_io_chip_init(&d->chip, &io, output, direction);
    if (err != ESP_OK) return err;
    const solar_os_gpio_controller_registration_t registration = {
        .name = d->name, .line_count = 8, .ops = &ops, .ctx = d,
    };
    err = solar_os_gpio_controller_register(&registration);
    if (err != ESP_OK) return err;
    if (d->battery) {
        const solar_os_battery_provider_t provider = {.read = battery_read, .user = d};
        err = solar_os_battery_register_provider(d->name, &provider);
        if (err != ESP_OK) {
            (void)solar_os_gpio_controller_unregister(d->name);
            return err;
        }
    }
    if (d->backlight) err = solar_os_backlight_register(backlight_set, d);
    if (err != ESP_OK) {
        if (d->battery) (void)solar_os_battery_unregister_provider(d->name);
        (void)solar_os_gpio_controller_unregister(d->name);
        return err;
    }
    d->active = true;
    return ESP_OK;
}

esp_err_t solar_os_waveshare_io_detach(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 2; i++) {
        ws_io_device_t *d = &devices[i];
        if (!d->active || strcmp(d->name, name)) continue;
        /* Controller unregister refuses while a consumer owns any line. */
        esp_err_t err = solar_os_gpio_controller_unregister(name);
        if (err != ESP_OK) return err;
        if (d->backlight) solar_os_backlight_unregister(d);
        if (d->battery) (void)solar_os_battery_unregister_provider(name);
        memset(d, 0, sizeof(*d));
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
