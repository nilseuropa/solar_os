#include <assert.h>
#include <stdio.h>
#include <string.h>

/* An auxiliary round panel must not inherit an unrelated primary panel. */
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH 168
#define SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT 384
#define SOLAR_OS_BOARD_DISPLAY_MADCTL 0x60
#define SOLAR_OS_BOARD_DISPLAY_COL_OFFSET 12
#define SOLAR_OS_BOARD_DISPLAY_ROW_OFFSET 24
#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R3
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_PULSE_STEPS 16
#define SOLAR_OS_BOARD_LCD_BACKLIGHT_STEP_PERCENT 20
#define SOLAR_OS_BOARD_LCD_POWER_ACTIVE_LEVEL 0
#include "../../src/services/solar_os_tft_display.c"

const u8g2_cb_t u8g2_cb_r0 = {0}, u8g2_cb_r1 = {0}, u8g2_cb_r2 = {0}, u8g2_cb_r3 = {0};
static unsigned initialized, deinited;
static bool primary;
static esp_err_t init_error, register_error, unregister_error;
static tft_ili9341_config_t received;
static solar_os_display_target_t target;
static solar_os_board_display_t *primary_display;
static const solar_os_display_raster_t *received_frame;
static const solar_os_display_surface_t *received_surface;

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(dst, src, count);
        dst[count] = 0;
    }
    return length;
}
bool solar_os_display_target_is_board_primary(const char *name) { (void)name; return primary; }
esp_err_t solar_os_display_register_target(const solar_os_display_target_t *value)
{ target = *value; return register_error; }
esp_err_t solar_os_display_unregister_target(const char *name)
{ assert(!strcmp(name, target.name)); return unregister_error; }
esp_err_t solar_os_board_display_register_primary(solar_os_board_display_t *display)
{ primary_display = display; return register_error; }
esp_err_t solar_os_board_display_unregister_primary(solar_os_board_display_t *display)
{ assert(display == primary_display); return unregister_error; }
esp_err_t tft_ili9341_init(tft_ili9341_t *display, const tft_ili9341_config_t *config)
{
    initialized++;
    received = *config;
    display->config = *config;
    display->u8g2.width = config->width;
    display->u8g2.height = config->height;
    return init_error;
}
void tft_ili9341_deinit(tft_ili9341_t *display) { (void)display; deinited++; }
esp_err_t tft_ili9341_resume(tft_ili9341_t *display) { (void)display; return ESP_OK; }
esp_err_t tft_ili9341_shutdown(tft_ili9341_t *display) { (void)display; return ESP_OK; }
u8g2_t *tft_ili9341_get_u8g2(tft_ili9341_t *display) { return &display->u8g2; }
bool tft_ili9341_backlight_supported(void) { return received.backlight_pin >= 0; }
esp_err_t tft_ili9341_get_backlight(const tft_ili9341_t *display, uint8_t *percent)
{ (void)display; *percent = 100; return ESP_OK; }
esp_err_t tft_ili9341_set_backlight(tft_ili9341_t *display, uint8_t percent)
{ (void)display; (void)percent; return ESP_OK; }
esp_err_t tft_ili9341_set_colors(tft_ili9341_t *display, uint32_t fg, uint32_t bg)
{ (void)display; (void)fg; (void)bg; return ESP_OK; }
esp_err_t tft_ili9341_present_surface(tft_ili9341_t *display, const solar_os_display_surface_t *surface)
{ assert(display == &device->driver); received_surface = surface; return ESP_OK; }
esp_err_t tft_ili9341_present_frame(tft_ili9341_t *display, const solar_os_display_raster_t *frame)
{ assert(display == &device->driver); received_frame = frame; return ESP_OK; }

int main(void)
{
    const solar_os_expansion_driver_t *driver = &solar_os_gc9a01_expansion_driver;
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .target = "spi0", .value = 10},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = 4},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 5},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bl", .value = 6},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power", .value = 7},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pwm", .value = 1},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "clock", .value = 20000},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .value = 3},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "bgr", .value = 0},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "invert", .value = 0},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "active", .value = 0},
    };
    const size_t count = sizeof(bindings) / sizeof(bindings[0]);
    assert(driver->attach("round0", bindings, 2) == ESP_ERR_INVALID_ARG);
    assert(initialized == 0 && device == NULL);
    assert(driver->attach("round0", bindings, 3) == ESP_OK);
    assert(received.gc9a01 && !received.st7789 && !received.st7796);
    assert(!strcmp(received.spi_bus, "spi0") && received.cs_pin == 10 && received.dc_pin == 4);
    assert(received.reset_pin == -1 && received.backlight_pin == -1 && received.power_pin == -1);
    assert(received.width == 240 && received.height == 240);
    assert(received.col_offset == 0 && received.row_offset == 0);
    assert(received.spi_clock_hz == 40000000 && received.madctl == 0x08);
    assert(received.invert_color && received.rotation == U8G2_R0);
    assert(received.backlight_pulse_steps == 0 && received.backlight_step_percent == 0);
    assert(!strcmp(target.driver, "gc9a01") && !strcmp(target.controller, "GC9A01"));
    assert(target.width == 240 && target.height == 240 && !target.brightness_supported);
    assert(target.frame_formats & SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT);
    const solar_os_display_raster_t frame = {.format = SOLAR_OS_DISPLAY_FORMAT_RGB565};
    const solar_os_display_surface_t surface = {.format = SOLAR_OS_DISPLAY_FORMAT_INDEX8};
    assert(target.present_frame(target.frame_context, &frame) == ESP_OK && received_frame == &frame);
    assert(target.present_surface(target.surface_context, &surface) == ESP_OK && received_surface == &surface);
    assert(driver->attach("other", bindings, 3) == ESP_ERR_INVALID_STATE);
    assert(driver->detach("other") == ESP_ERR_NOT_FOUND);
    unregister_error = ESP_ERR_INVALID_STATE;
    assert(driver->detach("round0") == unregister_error && device != NULL);
    unregister_error = ESP_OK;
    assert(driver->detach("round0") == ESP_OK && device == NULL && deinited == 1);

    assert(driver->attach("round0", bindings, count) == ESP_OK);
    assert(received.rotation == U8G2_R3 && received.madctl == 0 && !received.invert_color);
    assert(received.spi_clock_hz == 20000000 && received.backlight_pwm);
    assert(received.backlight_pin == 6 && received.reset_pin == 5 && received.power_pin == 7);
    assert(!received.backlight_active_high && received.power_active_high);
    assert(target.brightness_supported);
    assert(driver->detach("round0") == ESP_OK);
    bindings[7].value = 0;
    assert(driver->attach("round0", bindings, count) == ESP_ERR_INVALID_ARG && device == NULL);
    bindings[7].value = 40001;
    assert(driver->attach("round0", bindings, count) == ESP_ERR_INVALID_ARG);
    bindings[7].value = 20000;
    bindings[8].value = 4;
    assert(driver->attach("round0", bindings, count) == ESP_ERR_INVALID_ARG);
    bindings[8].value = 0;
    bindings[9].value = 2;
    assert(driver->attach("round0", bindings, count) == ESP_ERR_INVALID_ARG);
    bindings[9].value = 1;

    init_error = ESP_FAIL;
    assert(driver->attach("round0", bindings, count) == ESP_FAIL && device == NULL);
    init_error = ESP_OK;
    unsigned before = deinited;
    register_error = ESP_ERR_NO_MEM;
    assert(driver->attach("round0", bindings, count) == register_error && device == NULL);
    assert(deinited == before + 1);
    register_error = ESP_OK;
    primary = true;
    assert(driver->attach("display0", bindings, count) == ESP_OK);
    assert(primary_display->width == 240 && primary_display->height == 240);
    assert(driver->detach("display0") == ESP_OK && device == NULL);
    puts("gc9a01_expansion_test: ok");
}
