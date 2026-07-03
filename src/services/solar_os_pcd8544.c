#include "solar_os_pcd8544.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "pcd8544.h"
#include "solar_os_display.h"

#define SOLAR_OS_PCD8544_MAX 2

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int dc_pin;
    int reset_pin;
    pcd8544_t display;
} solar_os_pcd8544_device_t;

static const char *TAG = "pcd8544";
static solar_os_pcd8544_device_t devices[SOLAR_OS_PCD8544_MAX];

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static solar_os_pcd8544_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_PCD8544_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_pcd8544_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_PCD8544_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static void clear_device(solar_os_pcd8544_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->active) {
        pcd8544_deinit(&device->display);
    }
    memset(device, 0, sizeof(*device));
    device->cs_pin = -1;
    device->dc_pin = -1;
    device->reset_pin = -1;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *dc_pin,
                                int *reset_pin)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL ||
        spi_bus == NULL ||
        cs_pin == NULL ||
        dc_pin == NULL ||
        reset_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus[0] = '\0';
    *cs_pin = -1;
    *dc_pin = -1;
    *reset_pin = -1;

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(spi_bus, binding->target, spi_bus_len);
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            *cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, spi_bus_len);
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (binding_role_is(binding, "dc")) {
                if (*dc_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                *dc_pin = binding->value;
            } else if (binding_role_is(binding, "reset") || binding_role_is(binding, "rst")) {
                if (*reset_pin >= 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                *reset_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi ||
        !have_cs ||
        *dc_pin < 0 ||
        *reset_pin < 0 ||
        *cs_pin == *dc_pin ||
        *cs_pin == *reset_pin ||
        *dc_pin == *reset_pin ||
        !solar_os_expansion_find_spi_bus(spi_bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t register_display_target(solar_os_pcd8544_device_t *device)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, device->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, "pcd8544", sizeof(target.driver));
    strlcpy(target.controller, "PCD8544", sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = PCD8544_WIDTH;
    target.height = PCD8544_HEIGHT;
    target.ready = true;
    target.brightness_supported = false;
    target.black_is_one = true;
    target.u8g2 = pcd8544_get_u8g2(&device->display);
    return solar_os_display_register_target(&target);
}

esp_err_t solar_os_pcd8544_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    int cs_pin = -1;
    int dc_pin = -1;
    int reset_pin = -1;

    if (name == NULL ||
        name[0] == '\0' ||
        strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
        find_device(name) != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       spi_bus,
                                       sizeof(spi_bus),
                                       &cs_pin,
                                       &dc_pin,
                                       &reset_pin),
                        TAG,
                        "invalid bindings");

    solar_os_pcd8544_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->cs_pin = cs_pin;
    device->dc_pin = dc_pin;
    device->reset_pin = reset_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));

    esp_err_t ret = pcd8544_init(&device->display, cs_pin, dc_pin, reset_pin);
    if (ret == ESP_OK) {
        ret = register_display_target(device);
    }
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s CS GPIO%d DC GPIO%d RST GPIO%d",
             name,
             spi_bus,
             cs_pin,
             dc_pin,
             reset_pin);
    return ESP_OK;
}

esp_err_t solar_os_pcd8544_detach(const char *name)
{
    solar_os_pcd8544_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_display_unregister_target(name), TAG, "unregister display target failed");
    clear_device(device);
    return ESP_OK;
}
