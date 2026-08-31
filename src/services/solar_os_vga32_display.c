#include "solar_os_vga32_display.h"

#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"
#include "solar_os_resources.h"
#include "vga32.h"

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    vga32_t driver;
    solar_os_board_display_t display;
} vga32_device_t;

static vga32_device_t *device;

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL ? vga32_start_async_present(display->driver) :
        ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL ? vga32_resume(display->driver) :
        ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        vga32_deinit(display->driver);
        display->ready = false;
    }
}

static esp_err_t set_colors(solar_os_board_display_t *display,
                            uint32_t foreground,
                            uint32_t background)
{
    return display != NULL ?
        vga32_set_colors(display->driver, foreground, background) :
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
    return display != NULL ? vga32_present_mono_xbm(display->driver,
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
    return vga32_present_mono_xbm(
        context, frame->data, frame->data_size, frame->x, frame->y,
        frame->width, frame->height, frame->source_stride,
        frame->palette_inverted);
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .set_colors = set_colors,
    .present_mono_xbm = present_mono,
    .present_frame = present_frame,
};

static int *binding_pin(vga32_config_t *config, const char *role)
{
    if (strcmp(role, "r0") == 0) return &config->red0_pin;
    if (strcmp(role, "r1") == 0) return &config->red1_pin;
    if (strcmp(role, "g0") == 0) return &config->green0_pin;
    if (strcmp(role, "g1") == 0) return &config->green1_pin;
    if (strcmp(role, "b0") == 0) return &config->blue0_pin;
    if (strcmp(role, "b1") == 0) return &config->blue1_pin;
    if (strcmp(role, "hsync") == 0) return &config->hsync_pin;
    if (strcmp(role, "vsync") == 0) return &config->vsync_pin;
    return NULL;
}

static esp_err_t attach(const char *name,
                        const solar_os_expansion_binding_t *bindings,
                        size_t binding_count)
{
    vga32_config_t config = {
        .red0_pin = -1, .red1_pin = -1,
        .green0_pin = -1, .green1_pin = -1,
        .blue0_pin = -1, .blue1_pin = -1,
        .hsync_pin = -1, .vsync_pin = -1,
        .rotation = U8G2_R1,
    };
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_GPIO) {
            return ESP_ERR_INVALID_ARG;
        }
        int *pin = binding_pin(&config, bindings[i].role);
        if (pin == NULL || *pin >= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        *pin = bindings[i].value;
    }
    if (config.red0_pin < 0 || config.red1_pin < 0 ||
        config.green0_pin < 0 || config.green1_pin < 0 ||
        config.blue0_pin < 0 || config.blue1_pin < 0 ||
        config.hsync_pin < 0 || config.vsync_pin < 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(solar_os_resource_claim(SOLAR_OS_RESOURCE_I2S_PORT,
                                                1,
                                                -1,
                                                name,
                                                "vga-i2s1"),
                        "vga32",
                        "I2S1 claim failed");
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = vga32_init(&device->driver, &config);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "vga32",
        .u8g2 = vga32_get_u8g2(&device->driver),
        .controller = "VGA32",
        .width = VGA32_WIDTH,
        .height = VGA32_HEIGHT,
        .frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT,
        .preferred_stream_fps = 30,
        .max_stream_pixels_per_second = 2400000U,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    if (device->primary) {
        ret = solar_os_board_display_register_primary(&device->display);
    } else {
        solar_os_display_target_t target = {0};
        strlcpy(target.name, name, sizeof(target.name));
        strlcpy(target.source, "expansion", sizeof(target.source));
        strlcpy(target.driver, "vga32", sizeof(target.driver));
        strlcpy(target.controller, "VGA32", sizeof(target.controller));
        strlcpy(target.role, "aux", sizeof(target.role));
        target.width = VGA32_WIDTH;
        target.height = VGA32_HEIGHT;
        target.ready = true;
        target.frame_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
        target.preferred_stream_fps = 30;
        target.max_stream_pixels_per_second = 2400000U;
        target.u8g2 = device->display.u8g2;
        target.frame_context = &device->driver;
        target.present_frame = target_present_frame;
        ret = solar_os_display_register_target(&target);
    }
    if (ret != ESP_OK) {
        vga32_deinit(&device->driver);
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
        strcmp(name, device->name) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_err_t ret = device->primary ?
        solar_os_board_display_unregister_primary(&device->display) :
        solar_os_display_unregister_target(name);
    if (ret != ESP_OK) {
        return ret;
    }
    vga32_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "r0", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "r0", .required = true},
    {.key = "r1", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "r1", .required = true},
    {.key = "g0", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "g0", .required = true},
    {.key = "g1", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "g1", .required = true},
    {.key = "b0", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "b0", .required = true},
    {.key = "b1", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "b1", .required = true},
    {.key = "hsync", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "hsync", .required = true},
    {.key = "vsync", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "vsync", .required = true},
};

const solar_os_expansion_driver_t solar_os_vga32_expansion_driver = {
    .name = "vga32",
    .summary = "VGA RGB222 output",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach,
    .detach = detach,
};
