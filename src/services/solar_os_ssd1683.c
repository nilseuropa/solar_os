#include "solar_os_ssd1683.h"

#include <stdbool.h>
#include <string.h>

#include "epd_ssd1683.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

#define SOLAR_OS_SSD1683_MAX 2
#define SOLAR_OS_SSD1683_DEFAULT_SPI_CLOCK_KHZ 2000

typedef struct {
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    int power_pin;
    int spi_clock_khz;
    int rotation;
    epd_ssd1683_panel_variant_t panel_variant;
} ssd1683_binding_config_t;

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    epd_ssd1683_t driver;
    solar_os_board_display_t display;
} solar_os_ssd1683_device_t;

static const char *TAG = "ssd1683";
static solar_os_ssd1683_device_t *devices[SOLAR_OS_SSD1683_MAX];

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static int find_device_slot(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_SSD1683_MAX; i++) {
        if (devices[i] != NULL && devices[i]->active &&
            strcmp(devices[i]->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_device_slot(void)
{
    for (size_t i = 0; i < SOLAR_OS_SSD1683_MAX; i++) {
        if (devices[i] == NULL) {
            devices[i] = heap_caps_calloc(1,
                                          sizeof(*devices[i]),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            return devices[i] != NULL ? (int)i : -1;
        }
    }
    return -1;
}

static void release_device_slot(size_t slot)
{
    if (slot >= SOLAR_OS_SSD1683_MAX || devices[slot] == NULL) {
        return;
    }
    epd_ssd1683_deinit(&devices[slot]->driver);
    heap_caps_free(devices[slot]);
    devices[slot] = NULL;
}

static const u8g2_cb_t *rotation_callback(int rotation)
{
    switch (rotation) {
    case 0: return U8G2_R0;
    case 1: return U8G2_R1;
    case 2: return U8G2_R2;
    case 3: return U8G2_R3;
    default: return NULL;
    }
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                ssd1683_binding_config_t *config)
{
    bool have_spi = false;
    bool have_cs = false;
    if (bindings == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *config = (ssd1683_binding_config_t) {
        .cs_pin = -1,
        .dc_pin = -1,
        .reset_pin = -1,
        .busy_pin = -1,
        .power_pin = -1,
        .spi_clock_khz = SOLAR_OS_SSD1683_DEFAULT_SPI_CLOCK_KHZ,
        .rotation = -1,
        .panel_variant = EPD_SSD1683_PANEL_WAVESHARE_V2,
    };

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
            if (have_spi) {
                return ESP_ERR_INVALID_ARG;
            }
            strlcpy(config->spi_bus, binding->target, sizeof(config->spi_bus));
            have_spi = true;
            break;
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
            if (have_cs) {
                return ESP_ERR_INVALID_ARG;
            }
            config->cs_pin = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(config->spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(config->spi_bus, binding->target, sizeof(config->spi_bus));
                have_spi = true;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_GPIO:
            if (role_is(binding, "dc") && config->dc_pin < 0) {
                config->dc_pin = binding->value;
            } else if ((role_is(binding, "reset") || role_is(binding, "rst")) &&
                       config->reset_pin < 0) {
                config->reset_pin = binding->value;
            } else if (role_is(binding, "busy") && config->busy_pin < 0) {
                config->busy_pin = binding->value;
            } else if (role_is(binding, "power") && config->power_pin < 0) {
                config->power_pin = binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (role_is(binding, "clock")) {
                config->spi_clock_khz = binding->value;
            } else if (role_is(binding, "rotation")) {
                config->rotation = binding->value;
            } else if (role_is(binding, "panel")) {
                config->panel_variant = (epd_ssd1683_panel_variant_t)binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (config->rotation < 0) {
        config->rotation = config->panel_variant == EPD_SSD1683_PANEL_UNKNOWN ? 2 : 0;
    }

    if (!have_spi || !have_cs || config->dc_pin < 0 || config->reset_pin < 0 ||
        config->busy_pin < 0 || config->spi_clock_khz < 100 ||
        config->spi_clock_khz > 20000 || rotation_callback(config->rotation) == NULL ||
        config->panel_variant > EPD_SSD1683_PANEL_WAVESHARE_V2 ||
        config->cs_pin == config->dc_pin || config->cs_pin == config->reset_pin ||
        config->cs_pin == config->busy_pin || config->dc_pin == config->reset_pin ||
        config->dc_pin == config->busy_pin || config->reset_pin == config->busy_pin ||
        (config->power_pin >= 0 &&
         (config->power_pin == config->cs_pin || config->power_pin == config->dc_pin ||
          config->power_pin == config->reset_pin || config->power_pin == config->busy_pin)) ||
        !solar_os_expansion_find_spi_bus(config->spi_bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(config->spi_bus, config->cs_pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t display_runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t display_resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        epd_ssd1683_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        epd_ssd1683_deinit(display->driver);
        display->ready = false;
    }
}

static esp_err_t display_set_colors(solar_os_board_display_t *display,
                                    uint32_t foreground,
                                    uint32_t background)
{
    (void)display;
    (void)foreground;
    (void)background;
    return ESP_OK;
}

static const char *display_controller_mode(const solar_os_board_display_t *display)
{
    return display != NULL ? epd_ssd1683_controller_mode(display->driver) : NULL;
}

static const char *display_controller_mode_values(const solar_os_board_display_t *display)
{
    return display != NULL ? epd_ssd1683_controller_mode_values(display->driver) : NULL;
}

static esp_err_t display_set_controller_mode(solar_os_board_display_t *display,
                                             const char *mode)
{
    return display != NULL ? epd_ssd1683_set_controller_mode(display->driver, mode) :
        ESP_ERR_INVALID_STATE;
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = display_runtime_ready,
    .resume = display_resume,
    .deinit = display_deinit,
    .set_colors = display_set_colors,
    .controller_mode = display_controller_mode,
    .controller_mode_values = display_controller_mode_values,
    .set_controller_mode = display_set_controller_mode,
};

static const char *target_controller_mode(const void *context)
{
    return epd_ssd1683_controller_mode(context);
}

static const char *target_controller_mode_values(const void *context)
{
    return epd_ssd1683_controller_mode_values(context);
}

static esp_err_t target_set_controller_mode(void *context, const char *mode)
{
    return epd_ssd1683_set_controller_mode(context, mode);
}

static esp_err_t register_auxiliary(solar_os_ssd1683_device_t *device)
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
    target.u8g2 = device->display.u8g2;
    target.controller_context = &device->driver;
    target.controller_mode = target_controller_mode;
    target.controller_mode_values = target_controller_mode_values;
    target.set_controller_mode = target_set_controller_mode;
    return solar_os_display_register_target(&target);
}

esp_err_t solar_os_ssd1683_attach(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
        find_device_slot(name) >= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssd1683_binding_config_t binding_config;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &binding_config),
                        TAG,
                        "invalid bindings");

    const int slot = alloc_device_slot();
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_ssd1683_device_t *device = devices[slot];
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, binding_config.spi_bus, sizeof(device->spi_bus));

    const epd_ssd1683_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = binding_config.cs_pin,
        .dc_pin = binding_config.dc_pin,
        .reset_pin = binding_config.reset_pin,
        .busy_pin = binding_config.busy_pin,
        .power_pin = binding_config.power_pin,
        .spi_clock_hz = binding_config.spi_clock_khz * 1000,
        .busy_level = 1,
        .power_active_level = 1,
        .rotation = rotation_callback(binding_config.rotation),
        .panel_variant = binding_config.panel_variant,
    };
    esp_err_t ret = epd_ssd1683_init(&device->driver, &config);
    if (ret != ESP_OK) {
        release_device_slot((size_t)slot);
        return ret;
    }

    device->active = true;
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "ssd1683",
        .u8g2 = epd_ssd1683_get_u8g2(&device->driver),
        .controller = "SSD1683",
        .width = 400,
        .height = 300,
        .ready = true,
    };
    ret = device->primary ?
        solar_os_board_display_register_primary(&device->display) :
        register_auxiliary(device);
    if (ret != ESP_OK) {
        release_device_slot((size_t)slot);
        return ret;
    }

    ESP_LOGI(TAG,
             "%s attached on %s CS GPIO%d DC GPIO%d RST GPIO%d BUSY GPIO%d",
             name,
             device->spi_bus,
             binding_config.cs_pin,
             binding_config.dc_pin,
             binding_config.reset_pin,
             binding_config.busy_pin);
    return ESP_OK;
}

esp_err_t solar_os_ssd1683_detach(const char *name)
{
    const int slot = find_device_slot(name);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_ssd1683_device_t *device = devices[slot];
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    release_device_slot((size_t)slot);
    return ESP_OK;
}
