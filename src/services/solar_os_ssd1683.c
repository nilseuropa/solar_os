#include "solar_os_ssd1683.h"

#include <string.h>

#include "epd_ssd1683.h"
#include "esp_check.h"
#include "esp_log.h"
#include "solar_os_display.h"

#define SOLAR_OS_SSD1683_MAX 2
#define SOLAR_OS_SSD1683_SPI_CLOCK_HZ 2000000

typedef struct {
    bool active;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    epd_ssd1683_t display;
} solar_os_ssd1683_device_t;

static const char *TAG = "ssd1683";
static solar_os_ssd1683_device_t devices[SOLAR_OS_SSD1683_MAX];

static bool binding_role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static solar_os_ssd1683_device_t *find_device(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_SSD1683_MAX; i++) {
        if (devices[i].active && strcmp(devices[i].name, name) == 0) {
            return &devices[i];
        }
    }
    return NULL;
}

static solar_os_ssd1683_device_t *alloc_device(void)
{
    for (size_t i = 0; i < SOLAR_OS_SSD1683_MAX; i++) {
        if (!devices[i].active) {
            return &devices[i];
        }
    }
    return NULL;
}

static void clear_device(solar_os_ssd1683_device_t *device)
{
    if (device == NULL) {
        return;
    }
    if (device->active) {
        epd_ssd1683_deinit(&device->display);
    }
    memset(device, 0, sizeof(*device));
    device->cs_pin = -1;
    device->dc_pin = -1;
    device->reset_pin = -1;
    device->busy_pin = -1;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                char *spi_bus,
                                size_t spi_bus_len,
                                int *cs_pin,
                                int *dc_pin,
                                int *reset_pin,
                                int *busy_pin)
{
    bool have_spi = false;
    bool have_cs = false;

    if (bindings == NULL || spi_bus == NULL || cs_pin == NULL || dc_pin == NULL ||
        reset_pin == NULL || busy_pin == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    spi_bus[0] = '\0';
    *cs_pin = -1;
    *dc_pin = -1;
    *reset_pin = -1;
    *busy_pin = -1;

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
            if (binding_role_is(binding, "dc") && *dc_pin < 0) {
                *dc_pin = binding->value;
            } else if ((binding_role_is(binding, "reset") ||
                        binding_role_is(binding, "rst")) && *reset_pin < 0) {
                *reset_pin = binding->value;
            } else if (binding_role_is(binding, "busy") && *busy_pin < 0) {
                *busy_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (!have_spi || !have_cs || *dc_pin < 0 || *reset_pin < 0 || *busy_pin < 0 ||
        *cs_pin == *dc_pin || *cs_pin == *reset_pin || *cs_pin == *busy_pin ||
        *dc_pin == *reset_pin || *dc_pin == *busy_pin || *reset_pin == *busy_pin ||
        !solar_os_expansion_find_spi_bus(spi_bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(spi_bus, *cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static const char *display_controller_mode(const void *context)
{
    return epd_ssd1683_controller_mode((const epd_ssd1683_t *)context);
}

static const char *display_controller_mode_values(const void *context)
{
    return epd_ssd1683_controller_mode_values((const epd_ssd1683_t *)context);
}

static esp_err_t display_set_controller_mode(void *context, const char *mode)
{
    return epd_ssd1683_set_controller_mode((epd_ssd1683_t *)context, mode);
}

static esp_err_t register_display_target(solar_os_ssd1683_device_t *device)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, device->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, "ssd1683", sizeof(target.driver));
    strlcpy(target.controller, "UC8176", sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = 400;
    target.height = 300;
    target.ready = true;
    target.brightness_supported = false;
    target.black_is_one = false;
    target.u8g2 = epd_ssd1683_get_u8g2(&device->display);
    target.controller_context = &device->display;
    target.controller_mode = display_controller_mode;
    target.controller_mode_values = display_controller_mode_values;
    target.set_controller_mode = display_set_controller_mode;
    return solar_os_display_register_target(&target);
}

esp_err_t solar_os_ssd1683_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
    int cs_pin = -1;
    int dc_pin = -1;
    int reset_pin = -1;
    int busy_pin = -1;

    if (name == NULL || name[0] == '\0' ||
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
                                       &reset_pin,
                                       &busy_pin),
                        TAG,
                        "invalid bindings");

    solar_os_ssd1683_device_t *device = alloc_device();
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    clear_device(device);
    device->active = true;
    device->cs_pin = cs_pin;
    device->dc_pin = dc_pin;
    device->reset_pin = reset_pin;
    device->busy_pin = busy_pin;
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));

    const epd_ssd1683_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = cs_pin,
        .dc_pin = dc_pin,
        .reset_pin = reset_pin,
        .busy_pin = busy_pin,
        .power_pin = -1,
        .spi_clock_hz = SOLAR_OS_SSD1683_SPI_CLOCK_HZ,
        .busy_level = 1,
        .power_active_level = 1,
        .rotation = U8G2_R0,
        .panel_variant = EPD_SSD1683_PANEL_WAVESHARE_V2,
    };
    esp_err_t ret = epd_ssd1683_init(&device->display, &config);
    if (ret == ESP_OK) {
        ret = register_display_target(device);
    }
    if (ret != ESP_OK) {
        clear_device(device);
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s CS GPIO%d DC GPIO%d RST GPIO%d BUSY GPIO%d",
             name,
             spi_bus,
             cs_pin,
             dc_pin,
             reset_pin,
             busy_pin);
    return ESP_OK;
}

esp_err_t solar_os_ssd1683_detach(const char *name)
{
    solar_os_ssd1683_device_t *device = find_device(name);
    if (device == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    ESP_RETURN_ON_ERROR(solar_os_display_unregister_target(name),
                        TAG,
                        "unregister display target failed");
    clear_device(device);
    return ESP_OK;
}
