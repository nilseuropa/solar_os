#include <string.h>
#include "driver/gpio.h"
#include "epd_sticky_identify.h"
#include "esp_log.h"
#include "solar_os_ssd1677.h"
#include "solar_os_ssd2677.h"

static epd_sticky_controller_t controller;
static char device_name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || strlen(name) >= sizeof(device_name) || !bindings || count > 9 ||
        controller != EPD_STICKY_UNKNOWN) return ESP_ERR_INVALID_ARG;
    const char *bus = NULL;
    int cs = -1, pins[] = {-1,-1,-1,-1};
    bool parameter_seen[3] = {false};
    bool panel_seen = false;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS && !bus) bus = b->target;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS && cs < 0) cs = b->value;
        else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            static const char *const roles[] = {"dc","reset","busy","power"};
            unsigned r = 0;
            while (r < 4 && strcmp(roles[r], b->role)) r++;
            if (r == 4 || pins[r] >= 0 || b->value < 0) return ESP_ERR_INVALID_ARG;
            pins[r] = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            const int p = !strcmp(b->role, "clock") ? 0 : !strcmp(b->role, "rotation") ? 1 :
                !strcmp(b->role, "panel") ? 2 : -1;
            if (p < 0 || parameter_seen[p] || (p == 0 && (b->value < 100 || b->value > 20000)) ||
                (p == 1 && (b->value < 0 || b->value > 3)) || (p == 2 && b->value != 1))
                return ESP_ERR_INVALID_ARG;
            parameter_seen[p] = true;
            if (p == 2) panel_seen = true;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (!bus || !solar_os_expansion_find_spi_bus(bus, NULL, NULL) ||
        !solar_os_expansion_spi_cs_allowed(bus, cs)) return ESP_ERR_INVALID_ARG;
    epd_sticky_controller_t selected;
    esp_err_t err = epd_sticky_identify(bus, cs, pins[0], pins[1], pins[2], pins[3], &selected);
    if (err != ESP_OK) return err;
    solar_os_expansion_binding_t forwarded[10];
    memcpy(forwarded, bindings, count * sizeof(*bindings));
    if (!panel_seen) forwarded[count++] = (solar_os_expansion_binding_t){
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 1};
    err = selected == EPD_STICKY_SSD1677 ? solar_os_ssd1677_attach(name, forwarded, count) :
        solar_os_ssd2677_attach(name, forwarded, count);
    if (err == ESP_OK) {
        controller = selected;
        strlcpy(device_name, name, sizeof(device_name));
        ESP_LOGI("sticky-display", "%s detected %s", name,
            selected == EPD_STICKY_SSD1677 ? "SSD1677" : "SSD2677");
    }
    return err;
}

static esp_err_t detach(const char *name)
{
    if (!name || !device_name[0] || strcmp(name, device_name)) return ESP_ERR_NOT_FOUND;
    const esp_err_t err = controller == EPD_STICKY_SSD1677 ? solar_os_ssd1677_detach(name) :
        solar_os_ssd2677_detach(name);
    if (err == ESP_OK) { controller = EPD_STICKY_UNKNOWN; device_name[0] = '\0'; }
    return err;
}

static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="spi", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required=true},
    {.key="cs", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required=true},
    {.key="dc", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="dc", .required=true},
    {.key="reset", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="reset", .required=true},
    {.key="busy", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="busy", .required=true},
    {.key="power", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="power"},
    {.key="clock", .value_hint="khz", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="clock", .has_value_range=true, .min_value=100, .max_value=20000},
    {.key="rotation", .value_hint="0..3", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="rotation", .has_value_range=true, .min_value=0, .max_value=3},
    {.key="panel", .value_hint="1=Sticky", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="panel", .has_value_range=true, .min_value=1, .max_value=1},
};

const solar_os_expansion_driver_t solar_os_sticky_display_expansion_driver = {
    .name = "sticky-display", .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "Sticky SSD1677/SSD2677 automatic display",
    .required_capabilities = SOLAR_OS_BOARD_CAP_GFX | SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
        SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .early = true, .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = attach, .detach = detach,
};
