#include "solar_os_tft_display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"
#include "tft_ili9341.h"

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    tft_ili9341_t driver;
    solar_os_board_display_t display;
} tft_device_t;

static tft_device_t *device;

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count,
                                char *spi_bus,
                                int *cs,
                                int *dc,
                                int *reset,
                                int *backlight,
                                bool *backlight_active_high,
                                bool *backlight_pwm)
{
    bool have_spi = false;
    bool have_cs = false;
    spi_bus[0] = '\0';
    *cs = -1;
    *dc = -1;
    *reset = -1;
    *backlight = -1;
    *backlight_active_high = true;
    *backlight_pwm = false;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *binding = &bindings[i];
        if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !have_spi) {
            strlcpy(spi_bus, binding->target, SOLAR_OS_EXPANSION_TARGET_MAX);
            have_spi = true;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && !have_cs) {
            *cs = binding->value;
            have_cs = true;
            if (binding->target[0] != '\0') {
                if (have_spi && strcmp(spi_bus, binding->target) != 0) {
                    return ESP_ERR_INVALID_ARG;
                }
                strlcpy(spi_bus, binding->target, SOLAR_OS_EXPANSION_TARGET_MAX);
                have_spi = true;
            }
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "dc") && *dc < 0) {
            *dc = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "reset") && *reset < 0) {
            *reset = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
                   role_is(binding, "bl") && *backlight < 0) {
            *backlight = binding->value;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "active")) {
            *backlight_active_high = binding->value != 0;
        } else if (binding->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER &&
                   role_is(binding, "pwm")) {
            *backlight_pwm = binding->value != 0;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_spi && have_cs && *dc >= 0 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        tft_ili9341_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        tft_ili9341_deinit(display->driver);
        display->ready = false;
    }
}

static bool brightness_supported(const solar_os_board_display_t *display)
{
    (void)display;
    return tft_ili9341_backlight_supported();
}

static esp_err_t get_brightness(const solar_os_board_display_t *display, uint8_t *percent)
{
    return display != NULL ? tft_ili9341_get_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_brightness(solar_os_board_display_t *display, uint8_t percent)
{
    return display != NULL ? tft_ili9341_set_backlight(display->driver, percent) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    return display != NULL ?
        tft_ili9341_set_colors(display->driver, foreground, background) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_surface(solar_os_board_display_t *display,
                                 const solar_os_display_surface_t *surface)
{
    return display != NULL ?
        tft_ili9341_present_surface(display->driver, surface) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_auxiliary_surface(
    void *context,
    const solar_os_display_surface_t *surface)
{
    return tft_ili9341_present_surface((tft_ili9341_t *)context, surface);
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    return display != NULL ?
        tft_ili9341_present_frame(display->driver, frame) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_auxiliary_frame(
    void *context,
    const solar_os_display_raster_t *frame)
{
    return tft_ili9341_present_frame((tft_ili9341_t *)context, frame);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .brightness_supported = brightness_supported,
    .get_brightness = get_brightness,
    .set_brightness = set_brightness,
    .set_colors = set_colors,
    .present_surface = present_surface,
    .present_frame = present_frame,
};

static esp_err_t register_auxiliary(tft_device_t *attached)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, attached->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, attached->display.driver_name, sizeof(target.driver));
    strlcpy(target.controller, attached->display.controller, sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = attached->display.width;
    target.height = attached->display.height;
    target.ready = true;
    target.brightness_supported = brightness_supported(&attached->display);
    target.surface_formats = attached->display.surface_formats;
    target.frame_formats = attached->display.frame_formats;
    target.preferred_stream_fps = attached->display.preferred_stream_fps;
    target.max_stream_pixels_per_second =
        attached->display.max_stream_pixels_per_second;
    target.u8g2 = attached->display.u8g2;
    target.surface_context = &attached->driver;
    target.present_surface = present_auxiliary_surface;
    target.frame_context = &attached->driver;
    target.present_frame = present_auxiliary_frame;
    return solar_os_display_register_target(&target);
}

static esp_err_t attach_tft(const char *name,
                            const solar_os_expansion_binding_t *bindings,
                            size_t binding_count,
                            bool st7796)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs = -1;
    int dc = -1;
    int reset = -1;
    int backlight = -1;
    bool active_high = true;
    bool pwm = false;
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings,
                                       binding_count,
                                       spi_bus,
                                       &cs,
                                       &dc,
                                       &reset,
                                       &backlight,
                                       &active_high,
                                       &pwm),
                        "tft",
                        "invalid bindings");
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));
    const tft_ili9341_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = cs,
        .dc_pin = dc,
        .reset_pin = reset,
        .backlight_pin = backlight,
        .spi_clock_hz = 40000000U,
        .backlight_pwm_hz = 20000U,
        .width = st7796 ? 320 : 240,
        .height = st7796 ? 480 : 320,
        .madctl = st7796 ? 0x48 : 0x88,
        .st7796 = st7796,
        .backlight_active_high = active_high,
        .backlight_pwm = pwm,
        .rotation = U8G2_R1,
    };
    esp_err_t ret = tft_ili9341_init(&device->driver, &config);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = st7796 ? "st7796" : "ili9341",
        .u8g2 = tft_ili9341_get_u8g2(&device->driver),
        .controller = st7796 ? "ST7796" : "ILI9341",
        .width = st7796 ? 480 : 320,
        .height = st7796 ? 320 : 240,
        .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT,
        .preferred_stream_fps = st7796 ? 25 : 30,
        .max_stream_pixels_per_second = 1600000U,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    ret = device->primary ?
        solar_os_board_display_register_primary(&device->display) :
        register_auxiliary(device);
    if (ret != ESP_OK) {
        tft_ili9341_deinit(&device->driver);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->active = true;
    return ESP_OK;
}

static esp_err_t attach_ili9341(const char *name,
                                const solar_os_expansion_binding_t *bindings,
                                size_t binding_count)
{
    return attach_tft(name, bindings, binding_count, false);
}

static esp_err_t attach_st7796(const char *name,
                               const solar_os_expansion_binding_t *bindings,
                               size_t binding_count)
{
    return attach_tft(name, bindings, binding_count, true);
}

static esp_err_t detach(const char *name)
{
    if (device == NULL || !device->active || name == NULL ||
        strcmp(device->name, name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    tft_ili9341_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const int bool_values[] = {0, 1};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
    {.key = "bl", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl"},
    {.key = "active", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .allowed_values = bool_values, .allowed_value_count = 2},
    {.key = "pwm", .value_hint = "0|1", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .allowed_values = bool_values, .allowed_value_count = 2},
};

#define TFT_CAPABILITIES (SOLAR_OS_BOARD_CAP_GFX | \
                          SOLAR_OS_BOARD_CAP_EXPANSION_SPI | \
                          SOLAR_OS_BOARD_CAP_EXPANSION_GPIO)

const solar_os_expansion_driver_t solar_os_ili9341_expansion_driver = {
    .name = "ili9341",
    .summary = "320x240 color TFT",
    .required_capabilities = TFT_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_ili9341,
    .detach = detach,
};

const solar_os_expansion_driver_t solar_os_st7796_expansion_driver = {
    .name = "st7796",
    .summary = "480x320 color TFT",
    .required_capabilities = TFT_CAPABILITIES,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach_st7796,
    .detach = detach,
};
