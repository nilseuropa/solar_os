#include "solar_os_st7305_display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "rlcd_st7305.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    rlcd_st7305_t driver;
    solar_os_board_display_t display;
} st7305_device_t;

static st7305_device_t *device;

static bool role_is(const solar_os_expansion_binding_t *binding, const char *role)
{
    return binding != NULL && strcmp(binding->role, role) == 0;
}

static esp_err_t parse_bindings(const solar_os_expansion_binding_t *bindings,
                                size_t count,
                                char *spi_bus,
                                int *cs,
                                int *dc,
                                int *reset)
{
    bool have_spi = false;
    bool have_cs = false;
    *cs = -1;
    *dc = -1;
    *reset = -1;
    spi_bus[0] = '\0';
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
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    return have_spi && have_cs && *dc >= 0 && *reset >= 0 ?
        ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        rlcd_st7305_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        rlcd_st7305_deinit(display->driver);
        display->ready = false;
    }
}

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    (void)display;
    (void)foreground;
    (void)background;
    return ESP_OK;
}

static const char *controller_mode(const solar_os_board_display_t *display)
{
    return display != NULL ? rlcd_st7305_controller_mode(display->driver) : NULL;
}

static const char *controller_mode_values(const solar_os_board_display_t *display)
{
    return display != NULL ? rlcd_st7305_controller_mode_values(display->driver) : NULL;
}

static esp_err_t set_controller_mode(solar_os_board_display_t *display, const char *mode)
{
    return display != NULL ? rlcd_st7305_set_controller_mode(display->driver, mode) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t set_high_refresh(solar_os_board_display_t *display,
                                  bool enabled,
                                  uint16_t hz_tenths)
{
    return display != NULL ?
        rlcd_st7305_set_high_refresh_override(display->driver, enabled, hz_tenths) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_mono(solar_os_board_display_t *display,
                              const uint8_t *bitmap,
                              size_t bitmap_size,
                              uint16_t x,
                              uint16_t y,
                              uint16_t width,
                              uint16_t height,
                              uint16_t stride,
                              bool inverted)
{
    return display != NULL ? rlcd_st7305_present_mono_xbm(display->driver,
                                                          bitmap,
                                                          bitmap_size,
                                                          x,
                                                          y,
                                                          width,
                                                          height,
                                                          stride,
                                                          inverted) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t present_frame(solar_os_board_display_t *display,
                               const solar_os_display_raster_t *frame)
{
    if (display == NULL || frame == NULL ||
        frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        frame->source_width != frame->width ||
        frame->source_height != frame->height) {
        return ESP_ERR_INVALID_ARG;
    }
    return present_mono(display, frame->data, frame->data_size,
                        frame->x, frame->y, frame->width, frame->height,
                        frame->source_stride, frame->palette_inverted);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .set_colors = set_colors,
    .controller_mode = controller_mode,
    .controller_mode_values = controller_mode_values,
    .set_controller_mode = set_controller_mode,
    .set_high_refresh_override = set_high_refresh,
    .present_mono_xbm = present_mono,
    .present_frame = present_frame,
};

static esp_err_t target_present_frame(
    void *context,
    const solar_os_display_raster_t *frame)
{
    if (context == NULL || frame == NULL ||
        frame->format != SOLAR_OS_DISPLAY_FORMAT_MONO1 ||
        frame->source_width != frame->width ||
        frame->source_height != frame->height) {
        return ESP_ERR_INVALID_ARG;
    }
    return rlcd_st7305_present_mono_xbm(
        context, frame->data, frame->data_size, frame->x, frame->y,
        frame->width, frame->height, frame->source_stride,
        frame->palette_inverted);
}

static const char *target_mode(const void *context)
{
    return rlcd_st7305_controller_mode(context);
}

static const char *target_mode_values(const void *context)
{
    return rlcd_st7305_controller_mode_values(context);
}

static esp_err_t target_set_mode(void *context, const char *mode)
{
    return rlcd_st7305_set_controller_mode(context, mode);
}

static esp_err_t register_auxiliary(st7305_device_t *attached)
{
    solar_os_display_target_t target = {0};
    strlcpy(target.name, attached->name, sizeof(target.name));
    strlcpy(target.source, "expansion", sizeof(target.source));
    strlcpy(target.driver, "st7305", sizeof(target.driver));
    strlcpy(target.controller, "ST7305", sizeof(target.controller));
    strlcpy(target.role, "aux", sizeof(target.role));
    target.width = 400;
    target.height = 300;
    target.ready = true;
    target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    target.preferred_stream_fps = 25;
    target.max_stream_pixels_per_second = 2400000U;
    target.u8g2 = attached->display.u8g2;
    target.controller_context = &attached->driver;
    target.controller_mode = target_mode;
    target.controller_mode_values = target_mode_values;
    target.set_controller_mode = target_set_mode;
    target.frame_context = &attached->driver;
    target.present_frame = target_present_frame;
    return solar_os_display_register_target(&target);
}

static esp_err_t attach(const char *name,
                        const solar_os_expansion_binding_t *bindings,
                        size_t binding_count)
{
    char spi_bus[SOLAR_OS_EXPANSION_TARGET_MAX];
    int cs = -1;
    int dc = -1;
    int reset = -1;
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(parse_bindings(bindings, binding_count, spi_bus, &cs, &dc, &reset),
                        "st7305",
                        "invalid bindings");
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    strlcpy(device->spi_bus, spi_bus, sizeof(device->spi_bus));
    const rlcd_st7305_config_t config = {
        .spi_bus = device->spi_bus,
        .cs_pin = cs,
        .dc_pin = dc,
        .reset_pin = reset,
        .spi_clock_hz = 24000000U,
        .rotation = U8G2_R1,
    };
    esp_err_t ret = rlcd_st7305_init(&device->driver, &config);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "st7305",
        .u8g2 = rlcd_st7305_get_u8g2(&device->driver),
        .controller = "ST7305",
        .width = 400,
        .height = 300,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT,
        .preferred_stream_fps = 25,
        .max_stream_pixels_per_second = 2400000U,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    ret = device->primary ?
        solar_os_board_display_register_primary(&device->display) :
        register_auxiliary(device);
    if (ret != ESP_OK) {
        rlcd_st7305_deinit(&device->driver);
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    device->active = true;
    return ESP_OK;
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
    rlcd_st7305_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
};

const solar_os_expansion_driver_t solar_os_st7305_expansion_driver = {
    .name = "st7305",
    .summary = "400x300 reflective LCD",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX |
                             SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach,
    .detach = detach,
};
