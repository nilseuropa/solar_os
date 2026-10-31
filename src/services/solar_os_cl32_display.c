#include <string.h>

#include "cl32_display_identify.h"
#include "esp_log.h"
#include "solar_os_ssd1685.h"
#include "solar_os_st7305_display.h"

static cl32_display_controller_t controller;
static char device_name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings,
                        size_t count)
{
    if (controller != CL32_DISPLAY_UNKNOWN) return ESP_ERR_INVALID_STATE;
    if (name == NULL || name[0] == '\0' || strlen(name) >= sizeof(device_name) ||
        bindings == NULL || count != 5) return ESP_ERR_INVALID_ARG;
    const char *bus = NULL;
    int cs = -1, dc = -1, reset = -1, busy = -1;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && bus == NULL) bus = b->target;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && cs < 0) cs = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            int *pin = strcmp(b->role, "dc") == 0 ? &dc :
                strcmp(b->role, "reset") == 0 ? &reset :
                strcmp(b->role, "busy") == 0 ? &busy : NULL;
            if (pin == NULL || *pin >= 0 || b->value < 0) return ESP_ERR_INVALID_ARG;
            *pin = b->value;
        } else return ESP_ERR_INVALID_ARG;
    }
    solar_os_expansion_spi_bus_t spi_bus;
    if (bus == NULL || cs < 0 || dc < 0 || reset < 0 || busy < 0 ||
        !solar_os_expansion_find_spi_bus(bus, &spi_bus, NULL) ||
        !solar_os_expansion_spi_cs_allowed(bus, cs)) return ESP_ERR_INVALID_ARG;
    cl32_display_controller_t selected;
    esp_err_t err = cl32_display_identify(bus, cs, dc, reset, spi_bus.mosi_pin, &selected);
    if (err != ESP_OK) return err;
    if (selected != CL32_DISPLAY_ST7305 && selected != CL32_DISPLAY_SSD1685)
        return ESP_ERR_NOT_SUPPORTED;
    solar_os_expansion_binding_t forwarded[7];
    size_t forwarded_count = 0;
    for (size_t i = 0; i < count; i++) {
        if (selected == CL32_DISPLAY_ST7305 && bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_GPIO &&
            strcmp(bindings[i].role, "busy") == 0) continue;
        forwarded[forwarded_count++] = bindings[i];
    }
    forwarded[forwarded_count++] = (solar_os_expansion_binding_t) {
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 1,
    };
    forwarded[forwarded_count++] = (solar_os_expansion_binding_t) {
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .value = 3,
    };
    err = selected == CL32_DISPLAY_ST7305 ?
        solar_os_st7305_expansion_driver.attach(name, forwarded, forwarded_count) :
        solar_os_ssd1685_attach(name, forwarded, forwarded_count);
    if (err == ESP_OK) {
        controller = selected;
        strlcpy(device_name, name, sizeof(device_name));
        ESP_LOGI("cl32-display", "%s detected %s", name,
                 selected == CL32_DISPLAY_ST7305 ? "ST7305" : "SSD1685");
    }
    return err;
}

static esp_err_t detach(const char *name)
{
    if (name == NULL || device_name[0] == '\0' || strcmp(name, device_name) != 0)
        return ESP_ERR_NOT_FOUND;
    const esp_err_t err = controller == CL32_DISPLAY_ST7305 ?
        solar_os_st7305_expansion_driver.detach(name) : solar_os_ssd1685_detach(name);
    if (err == ESP_OK) { controller = CL32_DISPLAY_UNKNOWN; device_name[0] = '\0'; }
    return err;
}

static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "spi", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "busy", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
};

const solar_os_expansion_driver_t solar_os_cl32_display_expansion_driver = {
    .name = "cl32-display", .summary = "CL-32 automatic ST7305/SSD1685 display",
    .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY, .board_only = true, .early = true,
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX | SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
                             SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = attach, .detach = detach,
};
