#include "solar_os_ssd1306.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_display.h"
#include "ssd1306.h"

#define SOLAR_OS_SSD1306_MAX 2

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    uint8_t address;
    ssd1306_controller_t controller;
    ssd1306_t display;
} solar_os_ssd1306_device_t;

static const char *TAG = "ssd1306";
static solar_os_ssd1306_device_t devices[SOLAR_OS_SSD1306_MAX];

static solar_os_ssd1306_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_SSD1306_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_ssd1306_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_SSD1306_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static void clear_device(solar_os_ssd1306_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->active) {
        ssd1306_deinit(&device->display);
    }
    memset(device, 0, sizeof(*device));
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
    *address = 0;

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
            if (have_address || (binding->value != 0x3c && binding->value != 0x3d)) {
                return ESP_ERR_INVALID_ARG;
            }
            *address = (uint8_t)binding->value;
            have_address = true;
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_i2c ||
        !have_address ||
        !solar_os_expansion_find_i2c_bus(i2c_bus, NULL, NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t register_display_target(solar_os_ssd1306_device_t *device)
{
    const bool is_sh1106 = device->controller == SSD1306_CONTROLLER_SH1106;
    solar_os_display_target_t target = {0};
    strlcpy(target.name, device->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, is_sh1106 ? "sh1106" : "ssd1306", sizeof(target.driver));
    strlcpy(target.controller, is_sh1106 ? "SH1106" : "SSD1306", sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = SSD1306_WIDTH;
    target.height = SSD1306_HEIGHT;
    target.ready = true;
    target.brightness_supported = false;
    target.black_is_one = false;
    target.u8g2 = ssd1306_get_u8g2(&device->display);
    return solar_os_display_register_target(&target);
}

static esp_err_t attach_display(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                ssd1306_controller_t controller)
{
    char i2c_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    uint8_t address = 0;

    if (name == NULL ||
        name[0] == '\0' ||
        strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
        find_device(name) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       i2c_bus,
                                       sizeof(i2c_bus),
                                       &address),
                        TAG,
                        "invalid bindings");

    solar_os_ssd1306_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->address = address;
    device->controller = controller;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->i2c_bus, i2c_bus, sizeof(device->i2c_bus));

    esp_err_t ret = ssd1306_init(&device->display, address, controller);
    if (ret == ESP_OK) {
        ret = register_display_target(device);
    }
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s address 0x%02x as %s",
             name,
             i2c_bus,
             address,
             controller == SSD1306_CONTROLLER_SH1106 ? "SH1106" : "SSD1306");
    return ESP_OK;
}

esp_err_t solar_os_ssd1306_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    return attach_display(name, bindings, binding_count, SSD1306_CONTROLLER_SSD1306);
}

esp_err_t solar_os_sh1106_attach(const char *name,
                                 const solar_os_expansion_binding_t *bindings,
                                 size_t binding_count)
{
    return attach_display(name, bindings, binding_count, SSD1306_CONTROLLER_SH1106);
}

esp_err_t solar_os_ssd1306_detach(const char *name)
{
    solar_os_ssd1306_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_display_unregister_target(name), TAG, "unregister display target failed");
    clear_device(device);
    return ESP_OK;
}
