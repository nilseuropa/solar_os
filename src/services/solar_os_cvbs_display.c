#include "solar_os_cvbs_display.h"

#include <string.h>

#include "cvbs_pal.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"

typedef struct {
    bool active;
    bool primary;
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    cvbs_pal_t driver;
    solar_os_board_display_t display;
} cvbs_device_t;

static cvbs_device_t *device;

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display != NULL && display->driver != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t ret = display != NULL && display->driver != NULL ?
        cvbs_pal_resume(display->driver) : ESP_ERR_INVALID_STATE;
    if (display != NULL) {
        display->ready = ret == ESP_OK;
    }
    return ret;
}

static void deinit(solar_os_board_display_t *display)
{
    if (display != NULL && display->driver != NULL) {
        cvbs_pal_deinit(display->driver);
        display->ready = false;
    }
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
    (void)inverted;
    return display != NULL ? cvbs_pal_present_mono_xbm(display->driver,
                                                       bitmap,
                                                       bitmap_size,
                                                       x,
                                                       y,
                                                       width,
                                                       height,
                                                       stride,
                                                       false) :
        ESP_ERR_INVALID_STATE;
}

static const solar_os_board_display_ops_t display_ops = {
    .runtime_ready = runtime_ready,
    .resume = resume,
    .deinit = deinit,
    .present_mono_xbm = present_mono,
};

static esp_err_t attach(const char *name,
                        const solar_os_expansion_binding_t *bindings,
                        size_t binding_count)
{
    int output_pin = -1;
    int i2s_port = -1;
    if (device != NULL || name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
            strcmp(bindings[i].role, "out") == 0 && output_pin < 0) {
            output_pin = bindings[i].value;
        } else if (bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_I2S_PORT &&
                   i2s_port < 0) {
            i2s_port = bindings[i].value;
        } else {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (output_pin != 25 || i2s_port != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const cvbs_pal_config_t config = {
        .output_pin = output_pin,
        .rotation = U8G2_R1,
    };
    device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (device == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = cvbs_pal_init(&device->driver, &config);
    if (ret != ESP_OK) {
        heap_caps_free(device);
        device = NULL;
        return ret;
    }
    strlcpy(device->name, name, sizeof(device->name));
    device->display = (solar_os_board_display_t) {
        .ops = &display_ops,
        .driver = &device->driver,
        .driver_name = "cvbs-pal",
        .u8g2 = cvbs_pal_get_u8g2(&device->driver),
        .controller = "CVBS PAL",
        .width = CVBS_PAL_WIDTH,
        .height = CVBS_PAL_HEIGHT,
        .ready = true,
    };
    device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET) == 0;
    if (device->primary) {
        ret = solar_os_board_display_register_primary(&device->display);
    } else {
        solar_os_display_target_t target = {0};
        strlcpy(target.name, name, sizeof(target.name));
        strlcpy(target.source, "expansion", sizeof(target.source));
        strlcpy(target.driver, "cvbs-pal", sizeof(target.driver));
        strlcpy(target.controller, "CVBS PAL", sizeof(target.controller));
        strlcpy(target.role, "aux", sizeof(target.role));
        target.width = CVBS_PAL_WIDTH;
        target.height = CVBS_PAL_HEIGHT;
        target.ready = true;
        target.u8g2 = device->display.u8g2;
        ret = solar_os_display_register_target(&target);
    }
    if (ret != ESP_OK) {
        cvbs_pal_deinit(&device->driver);
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
    cvbs_pal_deinit(&device->driver);
    heap_caps_free(device);
    device = NULL;
    return ESP_OK;
}

static const int i2s_ports[] = {0};
static const int output_pins[] = {25};
static const solar_os_expansion_binding_spec_t binding_specs[] = {
    {.key = "i2s", .value_hint = "i2s0", .kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .required = true, .allowed_values = i2s_ports, .allowed_value_count = 1},
    {.key = "out", .value_hint = "gpio25", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "out", .required = true, .allowed_values = output_pins, .allowed_value_count = 1},
};

const solar_os_expansion_driver_t solar_os_cvbs_pal_expansion_driver = {
    .name = "cvbs-pal",
    .summary = "PAL composite video",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true,
    .binding_specs = binding_specs,
    .binding_spec_count = sizeof(binding_specs) / sizeof(binding_specs[0]),
    .attach = attach,
    .detach = detach,
};
