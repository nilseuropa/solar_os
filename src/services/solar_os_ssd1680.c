#include "solar_os_ssd1680.h"
#include "solar_os_sync.h"
#include "solar_os_display_bindings.h"

#include <stdbool.h>
#include <string.h>

#include "epd_ssd1680.h"
#include "esp_check.h"
#include "solar_os_memory.h"
#include "esp_log.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

#define SOLAR_OS_SSD1680_MAX 2
#define SOLAR_OS_SSD1680_DEFAULT_SPI_CLOCK_KHZ 4000

typedef struct {
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs_pin;
    int dc_pin;
    int reset_pin;
    int busy_pin;
    int power_pin;
    int spi_clock_khz;
    int rotation;
    unsigned panel_variant;
} ssd1680_binding_config_t;

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    epd_ssd1680_t driver;
    solar_os_board_display_t display;
} solar_os_ssd1680_device_t;

static solar_os_lifecycle_t lifecycle;

static const char *TAG = "ssd1680";
static solar_os_ssd1680_device_t *devices[SOLAR_OS_SSD1680_MAX];

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && role != NULL && strcmp(binding->role, role) == 0;
}

static int find_device_slot(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_SSD1680_MAX; i++) {
        if (devices[i] != NULL && devices[i]->active &&
            strcmp(devices[i]->name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_device_slot(void)
{
    for (size_t i = 0; i < SOLAR_OS_SSD1680_MAX; i++) {
        if (devices[i] == NULL) {
            devices[i] = solar_os_memory_calloc(1, sizeof(*devices[i]),
                SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "ssd1680.device");
            return devices[i] != NULL ? (int)i : -1;
        }
    }
    return -1;
}

static void release_device_slot(size_t slot)
{
    if (slot >= SOLAR_OS_SSD1680_MAX || devices[slot] == NULL) {
        return;
    }
    epd_ssd1680_deinit(&devices[slot]->driver);
    solar_os_memory_free(devices[slot]);
    devices[slot] = NULL;
}

static const u8g2_cb_t *rotation_callback(int rotation)
{
    return solar_os_display_rotation_callback(rotation);
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t binding_count,
                                ssd1680_binding_config_t *config)
{
    bool have_spi = false;
    bool have_cs = false;
    if (bindings == NULL || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *config = (ssd1680_binding_config_t) {
        .cs_pin = -1,
        .dc_pin = -1,
        .reset_pin = -1,
        .busy_pin = -1,
        .power_pin = -1,
        .spi_clock_khz = SOLAR_OS_SSD1680_DEFAULT_SPI_CLOCK_KHZ,
        .rotation = -1,
        .panel_variant = 0,
    };

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        switch (binding->kind) {
        case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        case SOLAR_OS_EXPANSION_BINDING_SPI_CS: {
            const esp_err_t err = solar_os_display_parse_spi_binding(binding,
                config->spi_bus, sizeof(config->spi_bus), &have_spi, &have_cs, &config->cs_pin);
            if (err != ESP_OK) return err;
            break;
        }
        case SOLAR_OS_EXPANSION_BINDING_GPIO: {
            const esp_err_t err = solar_os_display_parse_gpio_binding(binding,
                &config->dc_pin, &config->reset_pin, &config->busy_pin, &config->power_pin);
            if (err != ESP_OK) return err;
            break;
        }
        case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
            if (role_is(binding, "clock")) {
                config->spi_clock_khz = binding->value;
            } else if (role_is(binding, "rotation")) {
                config->rotation = binding->value;
            } else if (role_is(binding, "panel")) {
                config->panel_variant = (unsigned)binding->value;
            } else {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        default:
            return ESP_ERR_INVALID_ARG;
        }
    }

    if (config->rotation < 0) config->rotation = 0;

    if (!have_spi || !have_cs || config->dc_pin < 0 || config->reset_pin < 0 ||
        config->busy_pin < 0 || config->spi_clock_khz < 100 ||
        config->spi_clock_khz > 4000 || rotation_callback(config->rotation) == NULL ||
        config->panel_variant != EPD_SSD1680_PANEL_122X250 ||
        !solar_os_display_pins_valid(config->cs_pin, config->dc_pin,
            config->reset_pin, config->busy_pin, config->power_pin) ||
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
        epd_ssd1680_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void display_deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        epd_ssd1680_deinit(display->driver);
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
    return display != NULL ? epd_ssd1680_controller_mode(display->driver) : NULL;
}

static const char *display_controller_mode_values(const solar_os_board_display_t *display)
{
    return display != NULL ? epd_ssd1680_controller_mode_values(display->driver) : NULL;
}

static esp_err_t display_set_controller_mode(solar_os_board_display_t *display,
                                             const char *mode)
{
    return display != NULL ? epd_ssd1680_set_controller_mode(display->driver, mode) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_mono(solar_os_board_display_t *display,
                               const uint8_t *bitmap, size_t bitmap_size,
                               uint16_t x, uint16_t y, uint16_t width,
                               uint16_t height, uint16_t stride, bool inverted)
{
    return display != NULL ? epd_ssd1680_present_mono_xbm(display->driver,
        bitmap, bitmap_size, x, y, width, height, stride, inverted) : ESP_ERR_INVALID_STATE;
}

static esp_err_t target_present_frame(void *context, const solar_os_display_raster_t *frame)
{
    if (!frame || frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        frame->source_width != frame->width || frame->source_height != frame->height)
        return ESP_ERR_INVALID_ARG;
    return epd_ssd1680_present_mono_xbm(context, frame->data, frame->data_size,
        frame->x, frame->y, frame->width, frame->height, frame->source_stride, frame->palette_inverted);
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                                const solar_os_display_raster_t *frame)
{
    return display != NULL ? target_present_frame(display->driver, frame) : ESP_ERR_INVALID_STATE;
}

static esp_err_t target_relax(void *context, solar_os_display_relax_phase_t phase)
{
    return epd_ssd1680_relax(context, phase);
}

static esp_err_t display_relax(solar_os_board_display_t *display, solar_os_display_relax_phase_t phase)
{
    return display && display->driver ? target_relax(display->driver, phase) : ESP_ERR_INVALID_STATE;
}

static esp_err_t target_present_power_state(void *context)
{
    return epd_ssd1680_present_power_state(context);
}

static esp_err_t display_present_power_state(solar_os_board_display_t *display)
{
    return display ? target_present_power_state(display->driver) : ESP_ERR_INVALID_STATE;
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = display_runtime_ready,
    .resume = display_resume,
    .deinit = display_deinit,
    .set_colors = display_set_colors,
    .controller_mode = display_controller_mode,
    .controller_mode_values = display_controller_mode_values,
    .set_controller_mode = display_set_controller_mode,
    .relax = display_relax,
    .present_mono_xbm = present_mono,
    .present_frame = present_frame,
    .present_power_state = display_present_power_state,
};

static const char *target_controller_mode(const void *context)
{
    return epd_ssd1680_controller_mode(context);
}

static const char *target_controller_mode_values(const void *context)
{
    return epd_ssd1680_controller_mode_values(context);
}

static esp_err_t target_set_controller_mode(void *context, const char *mode)
{
    return epd_ssd1680_set_controller_mode(context, mode);
}

static esp_err_t register_auxiliary(solar_os_ssd1680_device_t *device)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, device->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, "ssd1680", sizeof(target.driver));
    strlcpy(target.controller, "SSD1680", sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = u8g2_GetDisplayWidth(device->display.u8g2);
    target.height = u8g2_GetDisplayHeight(device->display.u8g2);
    target.ready = true;
    target.u8g2 = device->display.u8g2;
    target.controller_context = &device->driver;
    target.controller_mode = target_controller_mode;
    target.controller_mode_values = target_controller_mode_values;
    target.set_controller_mode = target_set_controller_mode;
    target.relax = target_relax;
    target.relax_context = &device->driver;
    target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    target.preferred_stream_fps = 2U;
    target.present_frame = target_present_frame;
    target.frame_context = &device->driver;
    target.present_power_state = target_present_power_state;
    target.power_state_context = &device->driver;
    return solar_os_display_register_target(&target);
}

static esp_err_t attach_locked(const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count)
{
    if (name == NULL || name[0] == '\0' ||
        strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX ||
        find_device_slot(name) >= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ssd1680_binding_config_t binding_config;
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, &binding_config),
                        TAG,
                        "invalid bindings");

    const int slot = alloc_device_slot();
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    solar_os_ssd1680_device_t *device = devices[slot];
    strlcpy(device->name, name, sizeof(device->name));
    strlcpy(device->spi_bus, binding_config.spi_bus, sizeof(device->spi_bus));

    const epd_ssd1680_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = binding_config.cs_pin,
        .dc_pin = binding_config.dc_pin,
        .reset_pin = binding_config.reset_pin,
        .busy_pin = binding_config.busy_pin,
        .power_pin = binding_config.power_pin,
        .spi_clock_hz = binding_config.spi_clock_khz * 1000,
        .rotation = rotation_callback(binding_config.rotation),
        .panel = binding_config.panel_variant,
    };
    esp_err_t ret = epd_ssd1680_init(&device->driver, &config);
    if (ret != ESP_OK) {
        release_device_slot((size_t)slot);
        return ret;
    }

    device->active = true;
    device->primary = solar_os_display_target_is_board_primary(name);
    u8g2_t *const u8g2 = epd_ssd1680_get_u8g2(&device->driver);
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "ssd1680",
        .u8g2 = u8g2,
        .controller = "SSD1680",
        .width = u8g2_GetDisplayWidth(u8g2),
        .height = u8g2_GetDisplayHeight(u8g2),
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT,
        .preferred_stream_fps = 2U,
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

static esp_err_t detach_locked(const char *name)
{
    const int slot = find_device_slot(name);
    if (slot < 0) {
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_ssd1680_device_t *device = devices[slot];
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    release_device_slot((size_t)slot);
    return ESP_OK;
}

esp_err_t solar_os_ssd1680_attach(const char *name,
    const solar_os_expansion_binding_t *bindings, size_t binding_count)
{
    if (!solar_os_lifecycle_enter(&lifecycle)) return ESP_ERR_NO_MEM;
    const esp_err_t err = attach_locked(name, bindings, binding_count);
    solar_os_lifecycle_leave(&lifecycle);
    return err;
}

esp_err_t solar_os_ssd1680_detach(const char *name)
{
    if (!solar_os_lifecycle_enter(&lifecycle)) return ESP_ERR_NO_MEM;
    const esp_err_t err = detach_locked(name);
    solar_os_lifecycle_leave(&lifecycle);
    return err;
}
