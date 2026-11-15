#include "spd2010.h"
#include "solar_os_board_display.h"
#include "solar_os_display.h"
#include "solar_os_expansion.h"
#include "solar_os_memory.h"
#include "solar_os_resources.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX], owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    int host;
    bool primary, registered;
    solar_os_spd2010_t *driver;
    solar_os_board_display_t display;
} spd2010_device_t;
static spd2010_device_t *devices[2];

static esp_err_t ready(solar_os_board_display_t *b) { return b && b->ready ? ESP_OK : ESP_ERR_INVALID_STATE; }
static esp_err_t resume(solar_os_board_display_t *b)
{
    esp_err_t error = solar_os_spd2010_resume(b->driver);
    b->ready = error == ESP_OK; return error;
}
static esp_err_t shutdown(solar_os_board_display_t *b)
{
    esp_err_t error = solar_os_spd2010_shutdown(b->driver);
    if (!error) b->ready = false;
    return error;
}
static esp_err_t save_power(solar_os_board_display_t *b, bool enabled) { return solar_os_spd2010_power_save(b->driver, enabled); }
static bool brightness_supported(const solar_os_board_display_t *b) { return solar_os_spd2010_has_backlight(b->driver); }
static esp_err_t get_brightness(const solar_os_board_display_t *b, uint8_t *p) { return solar_os_spd2010_get_brightness(b->driver, p); }
static esp_err_t set_brightness(solar_os_board_display_t *b, uint8_t p) { return solar_os_spd2010_set_brightness(b->driver, p); }
static esp_err_t colors(solar_os_board_display_t *b, uint32_t fg, uint32_t bg) { return solar_os_spd2010_colors(b->driver, fg, bg); }
static esp_err_t frame(solar_os_board_display_t *b, const solar_os_display_raster_t *f) { return solar_os_spd2010_frame(b->driver, f); }
static esp_err_t surface(solar_os_board_display_t *b, const solar_os_display_surface_t *s) { return solar_os_spd2010_surface(b->driver, s); }
static void deinit(solar_os_board_display_t *b)
{
    if (b && b->driver && solar_os_spd2010_destroy(b->driver) == ESP_OK) {
        b->driver = NULL; b->u8g2 = NULL; b->ready = false;
    }
}
static const solar_os_board_display_ops_t ops = {
    .runtime_ready = ready, .resume = resume, .shutdown = shutdown, .deinit = deinit,
    .set_power_save = save_power, .brightness_supported = brightness_supported,
    .get_brightness = get_brightness, .set_brightness = set_brightness,
    .set_colors = colors, .present_frame = frame, .present_surface = surface,
};

static esp_err_t parse(const solar_os_expansion_binding_t *bindings, size_t count,
    solar_os_spd2010_config_t *c)
{
    if (!bindings) return ESP_ERR_INVALID_ARG;
    *c = (solar_os_spd2010_config_t){.host = -1, .sclk = -1, .data = {-1,-1,-1,-1},
        .cs = -1, .backlight = -1, .clock_hz = 20000000,
        .backlight_active_high = true, .power_active_high = true};
    for (size_t i = 0; i < count; ++i) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(b->role, bindings[j].role)) return ESP_ERR_INVALID_ARG;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            int *pin = NULL;
            if (!strcmp(b->role, "sclk")) pin = &c->sclk;
            else if (!strcmp(b->role, "cs")) pin = &c->cs;
            else if (!strcmp(b->role, "bl")) pin = &c->backlight;
            else if (strlen(b->role) == 2 && b->role[0] == 'd' && b->role[1] >= '0' && b->role[1] <= '3')
                pin = &c->data[b->role[1] - '0'];
            if (!pin) return ESP_ERR_INVALID_ARG;
            *pin = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO_LINE) {
            solar_os_gpio_line_ref_t *line;
            if (!strcmp(b->role, "power")) { line = &c->power; c->have_power = true; }
            else if (!strcmp(b->role, "reset")) { line = &c->reset; c->have_reset = true; }
            else return ESP_ERR_INVALID_ARG;
            if (b->value < 0 || b->value > UINT8_MAX || strlcpy(line->controller, b->target,
                sizeof(line->controller)) >= sizeof(line->controller)) return ESP_ERR_INVALID_ARG;
            line->line = b->value;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (!strcmp(b->role, "host") && b->value >= 1 && b->value <= 2) c->host = b->value;
            else if (!strcmp(b->role, "clock") && b->value >= 100 && b->value <= 40000) c->clock_hz = b->value * 1000U;
            else if (!strcmp(b->role, "rotation") && b->value >= 0 && b->value <= 3) c->rotation = b->value;
            else {
                if (b->value != 0 && b->value != 1) return ESP_ERR_INVALID_ARG;
                if (!strcmp(b->role, "bgr")) c->bgr = b->value;
                else if (!strcmp(b->role, "invert")) c->invert = b->value;
                else if (!strcmp(b->role, "pwm")) c->backlight_pwm = b->value;
                else if (!strcmp(b->role, "active")) c->backlight_active_high = b->value;
                else if (!strcmp(b->role, "power_active")) c->power_active_high = b->value;
                else return ESP_ERR_INVALID_ARG;
            }
        } else return ESP_ERR_INVALID_ARG;
    }
    if (c->host < 0 || (c->backlight_pwm && c->backlight < 0)) return ESP_ERR_INVALID_ARG;
    const int pins[] = {c->sclk, c->data[0], c->data[1], c->data[2], c->data[3], c->cs,
        c->backlight, c->have_power && !c->power.controller[0] ? c->power.line : -1,
        c->have_reset && !c->reset.controller[0] ? c->reset.line : -1};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        if (i < 6 && pins[i] < 0) return ESP_ERR_INVALID_ARG;
        for (size_t j = i + 1; j < sizeof(pins) / sizeof(pins[0]); ++j)
            if (pins[i] >= 0 && pins[i] == pins[j]) return ESP_ERR_INVALID_ARG;
    }
    if (c->have_power && c->have_reset && c->power.line == c->reset.line &&
        !strcmp(c->power.controller, c->reset.controller)) return ESP_ERR_INVALID_ARG;
    return ESP_OK;
}

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || strnlen(name, SOLAR_OS_DISPLAY_TARGET_NAME_MAX) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX)
        return ESP_ERR_INVALID_ARG;
    size_t slot = 2;
    for (size_t i = 0; i < 2; ++i) {
        if (devices[i] && !strcmp(devices[i]->name, name)) return ESP_ERR_INVALID_STATE;
        if (!devices[i]) slot = i;
    }
    if (slot == 2) return ESP_ERR_NO_MEM;
    solar_os_spd2010_config_t c;
    esp_err_t error = parse(bindings, count, &c);
    if (error) return error;
    spd2010_device_t *d = solar_os_memory_calloc(1, sizeof(*d), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "spd2010.device");
    if (!d) return ESP_ERR_NO_MEM;
    strlcpy(d->name, name, sizeof(d->name));
    snprintf(d->owner, sizeof(d->owner), "spd2010:%s", name);
    d->host = c.host;
    error = solar_os_resource_claim(SOLAR_OS_RESOURCE_SPI_HOST, c.host, -1, d->owner, "qspi-display");
    if (error) { solar_os_memory_free(d); return error; }
    error = solar_os_spd2010_create(&c, &d->driver);
    if (!error) {
        d->display = (solar_os_board_display_t){.ops = &ops, .driver = d->driver,
            .u8g2 = solar_os_spd2010_u8g2(d->driver), .driver_name = "spd2010", .controller = "SPD2010",
            .width = 412, .height = 412, .round = true, .ready = true,
            .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
            .frame_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT,
            .preferred_stream_fps = 20, .max_stream_pixels_per_second = 3400000};
        d->primary = solar_os_display_target_is_board_primary(name);
        if (d->primary) error = solar_os_board_display_register_primary(&d->display);
        else {
            solar_os_display_target_t t = {.width = 412, .height = 412, .native_width = 412, .native_height = 412,
                .ready = true, .round = true, .u8g2 = d->display.u8g2, .surface_formats = d->display.surface_formats,
                .frame_formats = d->display.frame_formats, .surface_context = d->driver, .frame_context = d->driver,
                .present_surface = solar_os_spd2010_surface, .present_frame = solar_os_spd2010_frame,
                .preferred_stream_fps = 20, .max_stream_pixels_per_second = 3400000,
                .brightness_supported = solar_os_spd2010_has_backlight(d->driver)};
            strlcpy(t.name, name, sizeof(t.name)); strlcpy(t.source, "expansion", sizeof(t.source));
            strlcpy(t.driver, "spd2010", sizeof(t.driver)); strlcpy(t.controller, "SPD2010", sizeof(t.controller));
            strlcpy(t.role, "aux", sizeof(t.role));
            error = solar_os_display_register_target(&t);
        }
    }
    if (error) {
        (void)solar_os_spd2010_destroy(d->driver);
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_SPI_HOST, c.host, -1, d->owner);
        solar_os_memory_free(d); return error;
    }
    d->registered = true; devices[slot] = d;
    return ESP_OK;
}

static esp_err_t detach(const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < 2; ++i) {
        spd2010_device_t *d = devices[i];
        if (!d || strcmp(name, d->name)) continue;
        esp_err_t error = ESP_OK;
        if (d->registered) error = d->primary ? solar_os_board_display_unregister_primary(&d->display) : solar_os_display_unregister_target(name);
        if (error) return error;
        d->registered = false;
        error = solar_os_spd2010_destroy(d->display.driver);
        if (error) return error;
        (void)solar_os_resource_release(SOLAR_OS_RESOURCE_SPI_HOST, d->host, -1, d->owner);
        solar_os_memory_free(d); devices[i] = NULL; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

#define PIN(key_, required_) {.key = key_, .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = key_, .required = required_}
#define PARAM(key_, min_, max_) {.key = key_, .value_hint = #min_ ".." #max_, .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = key_, .has_value_range = true, .min_value = min_, .max_value = max_}
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key = "host", .value_hint = "1=SPI2|2=SPI3", .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "host", .required = true, .has_value_range = true, .min_value = 1, .max_value = 2},
    PIN("sclk", true), PIN("d0", true), PIN("d1", true), PIN("d2", true), PIN("d3", true), PIN("cs", true), PIN("bl", false),
    {.key = "power", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power"},
    {.key = "reset", .value_hint = "gpio|controller:line", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "reset"},
    PARAM("clock", 100, 40000), PARAM("rotation", 0, 3), PARAM("bgr", 0, 1), PARAM("invert", 0, 1),
    PARAM("pwm", 0, 1), PARAM("active", 0, 1), PARAM("power_active", 0, 1),
};
#undef PIN
#undef PARAM
const solar_os_expansion_driver_t solar_os_spd2010_expansion_driver = {
    .name = "spd2010", .category = SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary = "SPD2010 412x412 QSPI color display",
    .early = true,
    .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO | SOLAR_OS_BOARD_CAP_PSRAM,
    .binding_specs = specs, .binding_spec_count = sizeof(specs) / sizeof(specs[0]),
    .attach = attach, .detach = detach,
};
