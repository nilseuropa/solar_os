#include <string.h>
#include "epd_it8951.h"
#include "solar_os_memory.h"
#include "solar_os_board_display.h"
#include "solar_os_buses.h"
#include "solar_os_display.h"
#include "solar_os_expansion.h"
#include "solar_os_sensors.h"

typedef struct {
    char name[SOLAR_OS_EXPANSION_DEVICE_NAME_MAX];
    solar_os_it8951_t driver;
    solar_os_board_display_t display;
    bool primary;
} it8951_device_t;

static it8951_device_t *devices[2];

static esp_err_t read_temperature(int16_t *temperature_c)
{
    float sample;
    const esp_err_t err = solar_os_sensors_read_temperature(NULL,&sample);
    if (err != ESP_OK) return err;
    /* Comparisons also reject NaN. Keep the declared temperature in the
     * ED103TC2 operating range; retain the last valid value on sensor failure. */
    if (!(sample >= 0 && sample <= 50)) return ESP_ERR_INVALID_RESPONSE;
    *temperature_c = (int16_t)(sample+0.5f);
    return ESP_OK;
}

static esp_err_t runtime_ready(solar_os_board_display_t *display)
{
    return display && ((solar_os_it8951_t *)display->driver)->ready ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t set_power_save(solar_os_board_display_t *display, bool sleep)
{
    return solar_os_it8951_set_power_save(display->driver, sleep);
}

static esp_err_t resume(solar_os_board_display_t *display)
{
    const esp_err_t err = set_power_save(display, false);
    display->ready = err == ESP_OK;
    return err;
}

static void deinit(solar_os_board_display_t *display)
{
    solar_os_it8951_deinit(display->driver);
    display->ready = false;
}

static const char *target_mode(const void *driver) { return solar_os_it8951_mode(driver); }
static const char *target_modes(const void *driver) { (void)driver; return "refresh=auto|full"; }
static esp_err_t target_set_mode(void *driver, const char *mode) { return solar_os_it8951_set_mode(driver, mode); }
static const char *mode(const solar_os_board_display_t *d) { return target_mode(d->driver); }
static const char *modes(const solar_os_board_display_t *d) { return target_modes(d->driver); }
static esp_err_t set_mode(solar_os_board_display_t *d, const char *s) { return target_set_mode(d->driver, s); }
static esp_err_t target_power_state(void *driver) { return solar_os_it8951_present_sync(driver); }
static esp_err_t power_state(solar_os_board_display_t *d) { return target_power_state(d->driver); }
static esp_err_t target_frame(void *driver, const solar_os_display_raster_t *f)
{ return solar_os_it8951_present_frame(driver, f); }
static esp_err_t frame(solar_os_board_display_t *d, const solar_os_display_raster_t *f)
{ return target_frame(d->driver, f); }
static esp_err_t target_surface(void *driver, const solar_os_display_surface_t *s)
{ return solar_os_it8951_present_surface(driver, s); }
static esp_err_t surface(solar_os_board_display_t *d, const solar_os_display_surface_t *s)
{ return target_surface(d->driver, s); }
static esp_err_t mono(solar_os_board_display_t *d, const uint8_t *bitmap, size_t size,
    uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t stride, bool inverted)
{
    const solar_os_display_raster_t f = {.data = bitmap, .data_size = size,
        .source_width = width, .source_height = height, .source_stride = stride,
        .x = x, .y = y, .width = width, .height = height,
        .format = SOLAR_OS_DISPLAY_FORMAT_MONO1, .palette_inverted = inverted};
    return frame(d, &f);
}

static const solar_os_board_display_ops_t ops = {
    .runtime_ready = runtime_ready, .resume = resume, .deinit = deinit,
    .set_power_save = set_power_save, .controller_mode = mode,
    .controller_mode_values = modes, .set_controller_mode = set_mode,
    .present_power_state = power_state, .present_mono_xbm = mono,
    .present_frame = frame, .present_surface = surface,
};

#define FRAME_FORMATS (SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT | SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT | \
    SOLAR_OS_DISPLAY_FORMAT_INDEX2_BIT | SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT)

static esp_err_t attach(const char *name, const solar_os_expansion_binding_t *bindings, size_t count)
{
    if (!name || !name[0] || strlen(name) >= SOLAR_OS_DISPLAY_TARGET_NAME_MAX || !bindings)
        return ESP_ERR_INVALID_ARG;
    unsigned slot = 2;
    for (unsigned i = 0; i < 2; i++) {
        if (devices[i] && !strcmp(devices[i]->name, name)) return ESP_ERR_INVALID_STATE;
        if (!devices[i] && slot == 2) slot = i;
    }
    if (slot == 2) return ESP_ERR_NO_MEM;
    solar_os_it8951_config_t cfg = {.cs = -1, .reset = -1, .busy = -1, .power = -1,
        .ite_power = -1, .clock_hz = 10000000, .vcom_mv = 1400,
        .read_temperature = read_temperature};
    unsigned seen = 0;
    const char *cs_bus = NULL;
    for (size_t i = 0; i < count; i++) {
        const solar_os_expansion_binding_t *b = &bindings[i];
        unsigned bit;
        if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_BUS) {
            bit = 1U; cfg.spi_bus = b->target;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_SPI_CS) {
            bit = 2U; cfg.cs = b->value;
            if (b->target[0]) cs_bus = b->target;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_GPIO) {
            if (!strcmp(b->role, "reset")) { bit = 4U; cfg.reset = b->value; }
            else if (!strcmp(b->role, "busy")) { bit = 8U; cfg.busy = b->value; }
            else if (!strcmp(b->role, "power")) { bit = 16U; cfg.power = b->value; }
            else if (!strcmp(b->role, "ite_power")) { bit = 32U; cfg.ite_power = b->value; }
            else return ESP_ERR_INVALID_ARG;
        } else if (b->kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if (!strcmp(b->role, "clock") && b->value >= 100 && b->value <= 10000) {
                bit = 64U; cfg.clock_hz = (unsigned)b->value * 1000U;
            } else if (!strcmp(b->role, "rotation") && b->value >= 0 && b->value <= 3) {
                bit = 128U; cfg.rotation = (unsigned)b->value;
            } else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
        if (seen & bit) return ESP_ERR_INVALID_ARG;
        seen |= bit;
    }
    solar_os_bus_info_t bus;
    if ((seen & 63U) != 63U ||
        (cs_bus && strcmp(cs_bus, cfg.spi_bus)) ||
        !solar_os_bus_find(cfg.spi_bus, SOLAR_OS_BUS_PROTOCOL_SPI, &bus) ||
        bus.config.spi.miso_pin < 0 || bus.config.spi.max_transfer_size < SOLAR_OS_IT8951_WIDTH ||
        !solar_os_expansion_spi_cs_allowed(cfg.spi_bus, cfg.cs)) return ESP_ERR_INVALID_ARG;
    it8951_device_t *device = solar_os_memory_calloc(1, sizeof(*device),
        SOLAR_OS_MEMORY_INTERNAL_CRITICAL, "it8951.device");
    if (!device) return ESP_ERR_NO_MEM;
    strlcpy(device->name, name, sizeof(device->name));
    esp_err_t err = solar_os_it8951_init(&device->driver, &cfg);
    if (err != ESP_OK) { solar_os_memory_free(device); return err; }
    device->primary = solar_os_display_target_is_board_primary(name);
    device->display = (solar_os_board_display_t){.ops = &ops, .driver = &device->driver,
        .driver_name = "it8951", .controller = "IT8951/ED103TC2", .u8g2 = &device->driver.u8g2,
        .width = u8g2_GetDisplayWidth(&device->driver.u8g2),
        .height = u8g2_GetDisplayHeight(&device->driver.u8g2), .ready = true,
        .frame_formats = FRAME_FORMATS, .surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT,
        .preferred_stream_fps = 1};
    if (device->primary) err = solar_os_board_display_register_primary(&device->display);
    else {
        solar_os_display_target_t target = {0};
        strlcpy(target.name, name, sizeof(target.name));
        strlcpy(target.source, "expansion", sizeof(target.source));
        strlcpy(target.driver, "it8951", sizeof(target.driver));
        strlcpy(target.controller, "IT8951/ED103TC2", sizeof(target.controller));
        strlcpy(target.role, "aux", sizeof(target.role));
        target.u8g2 = device->display.u8g2;
        target.width = device->display.width; target.height = device->display.height;
        target.ready = true;
        target.controller_context = &device->driver;
        target.controller_mode = target_mode; target.controller_mode_values = target_modes;
        target.set_controller_mode = target_set_mode;
        target.present_power_state = target_power_state; target.power_state_context = &device->driver;
        target.present_frame = target_frame; target.frame_context = &device->driver;
        target.frame_formats = FRAME_FORMATS;
        target.present_surface = target_surface; target.surface_context = &device->driver;
        target.surface_formats = SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
        target.preferred_stream_fps = 1;
        err = solar_os_display_register_target(&target);
    }
    if (err != ESP_OK) { solar_os_it8951_deinit(&device->driver); solar_os_memory_free(device); }
    else devices[slot] = device;
    return err;
}

static esp_err_t detach(const char *name)
{
    for (unsigned i = 0; name && i < 2; i++) {
        it8951_device_t *d = devices[i];
        if (!d || strcmp(d->name, name)) continue;
        const esp_err_t err = d->primary ? solar_os_board_display_unregister_primary(&d->display) :
            solar_os_display_unregister_target(name);
        if (err != ESP_OK) return err;
        solar_os_it8951_deinit(&d->driver);
        solar_os_memory_free(d); devices[i] = NULL;
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="spi", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required=true},
    {.key="cs", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required=true},
    {.key="reset", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="reset", .required=true},
    {.key="busy", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="busy", .required=true},
    {.key="power", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="power", .required=true},
    {.key="ite_power", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="ite_power", .required=true},
    {.key="clock", .value_hint="100..10000kHz", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="clock", .has_value_range=true, .min_value=100, .max_value=10000},
    {.key="rotation", .value_hint="0..3", .kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="rotation", .has_value_range=true, .min_value=0, .max_value=3},
};

const solar_os_expansion_driver_t solar_os_it8951_expansion_driver = {
    .name="it8951", .category=SOLAR_OS_EXPANSION_CATEGORY_DISPLAY,
    .summary="IT8951 ED103TC2 1872x1404 16-gray e-paper", .early=true,
    .required_capabilities=SOLAR_OS_BOARD_CAP_GFX | SOLAR_OS_BOARD_CAP_EXPANSION_SPI | SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
    .binding_specs=specs, .binding_spec_count=sizeof(specs)/sizeof(specs[0]),
    .attach=attach, .detach=detach,
};
