#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/services/solar_os_ssd1685.c"
#include "../../src/services/solar_os_ssd1685_driver.c"

static int initialized, deinited;
static esp_err_t init_error, register_error, unregister_error;
static bool primary;
static solar_os_display_target_t target;
static solar_os_board_display_t *primary_display;
static unsigned raster_calls;
static epd_ssd1685_t *raster_driver;
static solar_os_display_raster_t received;
static esp_err_t raster_error;
const u8g2_cb_t u8g2_cb_r0 = {0}, u8g2_cb_r1 = {0}, u8g2_cb_r2 = {0}, u8g2_cb_r3 = {0};

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{
    (void)bus; (void)index; return strcmp(name, "spi0") == 0;
}
bool solar_os_expansion_spi_cs_allowed(const char *name, int pin)
{
    return strcmp(name, "spi0") == 0 && pin == 10;
}
bool solar_os_display_target_is_board_primary(const char *name) { (void)name; return primary; }
esp_err_t solar_os_display_register_target(const solar_os_display_target_t *value)
{
    if (register_error) return register_error;
    target = *value; return ESP_OK;
}
esp_err_t solar_os_display_unregister_target(const char *name)
{
    assert(strcmp(name, target.name) == 0); return unregister_error;
}
esp_err_t solar_os_board_display_register_primary(solar_os_board_display_t *display)
{
    primary_display = display; return register_error;
}
esp_err_t solar_os_board_display_unregister_primary(solar_os_board_display_t *display)
{
    assert(display == primary_display); return unregister_error;
}
esp_err_t epd_ssd1685_init(epd_ssd1685_t *display, const epd_ssd1685_config_t *config)
{
    initialized++;
    assert(config->panel == 1 && config->spi_clock_hz == 10000000);
    assert(config->cs_pin == 10 && config->dc_pin == 17 && config->busy_pin == 15);
    assert(config->power_pin == -1 && config->rotation == U8G2_R0);
    display->u8g2.width = 168; display->u8g2.height = 384;
    return init_error;
}
void epd_ssd1685_deinit(epd_ssd1685_t *display) { (void)display; deinited++; }
esp_err_t epd_ssd1685_resume(epd_ssd1685_t *display) { (void)display; return ESP_OK; }
static epd_ssd1685_t *power_state_driver;
static esp_err_t power_state_error;
esp_err_t epd_ssd1685_present_power_state(epd_ssd1685_t *display)
{ power_state_driver = display; return power_state_error; }
u8g2_t *epd_ssd1685_get_u8g2(epd_ssd1685_t *display) { return &display->u8g2; }
const char *epd_ssd1685_controller_mode(const epd_ssd1685_t *display)
{ (void)display; return "refresh=auto"; }
const char *epd_ssd1685_controller_mode_values(const epd_ssd1685_t *display)
{ (void)display; return "refresh=<auto,partial,full>"; }
esp_err_t epd_ssd1685_set_controller_mode(epd_ssd1685_t *display, const char *mode)
{ (void)display; (void)mode; return ESP_OK; }

esp_err_t epd_ssd1685_present_mono_xbm(epd_ssd1685_t *display,
    const uint8_t *bitmap, size_t bitmap_size, uint16_t x, uint16_t y,
    uint16_t width, uint16_t height, uint16_t stride, bool inverted)
{
    ++raster_calls; raster_driver = display;
    received = (solar_os_display_raster_t){.data = bitmap, .data_size = bitmap_size,
        .x = x, .y = y, .width = width, .height = height, .source_stride = stride,
        .palette_inverted = inverted};
    return raster_error;
}

static void test_raster_forwarding(void *context,
    esp_err_t (*present)(void *, const solar_os_display_raster_t *))
{
    const uint8_t bitmap[4] = {0x15, 0xee, 0x0a, 0xff};
    solar_os_display_raster_t frame = {.format = SOLAR_OS_DISPLAY_FORMAT_MONO1,
        .data = bitmap, .data_size = sizeof(bitmap), .source_width = 5, .source_height = 2,
        .source_stride = 2, .x = 3, .y = 4, .width = 5, .height = 2, .palette_inverted = true};
    assert(present(context, &frame) == ESP_OK);
    assert(received.data == bitmap && received.data_size == sizeof(bitmap));
    assert(received.x == 3 && received.y == 4 && received.width == 5 && received.height == 2);
    assert(received.source_stride == 2 && received.palette_inverted);
    unsigned calls = raster_calls;
    frame.format = SOLAR_OS_DISPLAY_FORMAT_INDEX2;
    assert(present(context, &frame) == ESP_ERR_INVALID_ARG && raster_calls == calls);
    frame.format = SOLAR_OS_DISPLAY_FORMAT_MONO1;
    frame.source_width = 4;
    assert(present(context, &frame) == ESP_ERR_INVALID_ARG && raster_calls == calls);
    frame.source_width = 5; frame.source_height = 1;
    assert(present(context, &frame) == ESP_ERR_INVALID_ARG && raster_calls == calls);
    assert(present(context, NULL) == ESP_ERR_INVALID_ARG && raster_calls == calls);
    frame.source_height = 2; raster_error = ESP_FAIL;
    assert(present(context, &frame) == ESP_FAIL);
    raster_error = ESP_OK;
}

static esp_err_t primary_present(void *context, const solar_os_display_raster_t *frame)
{
    solar_os_board_display_t *display = context;
    return display->ops->present_frame(display, frame);
}

static void *relax_driver;
esp_err_t epd_ssd1685_relax(epd_ssd1685_t *d, solar_os_display_relax_phase_t phase)
{
    assert(phase == SOLAR_OS_DISPLAY_RELAX_BEGIN || phase == SOLAR_OS_DISPLAY_RELAX_END);
    relax_driver = d;
    return ESP_OK;
}

int main(void)
{
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value = 10},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = 17},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 16},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .value = 15},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 1},
    };
    assert(solar_os_ssd1685_attach("epd0", bindings, 5) == ESP_ERR_INVALID_ARG);
    assert(initialized == 0); /* no implicit panel selection */
    bindings[5].value = 0;
    assert(solar_os_ssd1685_attach("epd0", bindings, 6) == ESP_ERR_INVALID_ARG);
    bindings[5].value = 1;
    init_error = ESP_FAIL;
    assert(solar_os_ssd1685_attach("epd0", bindings, 6) == ESP_FAIL);
    assert(deinited == 1);
    init_error = ESP_OK; register_error = ESP_FAIL;
    assert(solar_os_ssd1685_attach("epd0", bindings, 6) == ESP_FAIL);
    assert(deinited == 2);
    register_error = ESP_OK;
    assert(solar_os_ssd1685_attach("epd0", bindings, 6) == ESP_OK);
    assert(target.width == 168 && target.height == 384 && !target.black_is_one);
    assert(strcmp(target.driver, "ssd1685") == 0 && strcmp(target.controller, "SSD1685") == 0);
    assert(target.frame_formats == SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT && target.preferred_stream_fps == 10);
    test_raster_forwarding(target.frame_context, target.present_frame);
    assert(raster_driver == target.frame_context);
    assert(target.present_power_state(target.power_state_context) == ESP_OK);
    assert(power_state_driver == target.frame_context);
    power_state_error = ESP_FAIL;
    assert(target.present_power_state(target.power_state_context) == ESP_FAIL);
    power_state_error = ESP_OK;
    assert(solar_os_ssd1685_attach("epd0", bindings, 6) == ESP_ERR_INVALID_ARG);
    assert(target.relax(target.relax_context, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(relax_driver == target.relax_context);
    unregister_error = ESP_FAIL;
    assert(solar_os_ssd1685_detach("epd0") == ESP_FAIL && deinited == 2);
    unregister_error = ESP_OK;
    assert(solar_os_ssd1685_detach("epd0") == ESP_OK && deinited == 3);
    assert(solar_os_ssd1685_detach("epd0") == ESP_ERR_NOT_FOUND);
    primary = true;
    assert(solar_os_ssd1685_attach("display0", bindings, 6) == ESP_OK);
    assert(primary_display->ready && strcmp(primary_display->controller, "SSD1685") == 0);
    assert(primary_display->frame_formats == SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT);
    assert(primary_display->preferred_stream_fps == 10 && primary_display->ops->present_mono_xbm);
    test_raster_forwarding(primary_display, primary_present);
    assert(raster_driver == primary_display->driver);
    assert(primary_display->ops->present_power_state(primary_display) == ESP_OK);
    assert(power_state_driver == primary_display->driver);
    power_state_error = ESP_FAIL;
    assert(primary_display->ops->present_power_state(primary_display) == ESP_FAIL);
    power_state_error = ESP_OK;
    const uint8_t mono = 1;
    assert(primary_display->ops->present_mono_xbm(primary_display, &mono, 1, 1, 2, 1, 1, 1, false) == ESP_OK);
    assert(received.data == &mono && received.x == 1 && received.y == 2 && !received.palette_inverted);
    assert(primary_display->ops->present_frame(NULL, NULL) == ESP_ERR_INVALID_STATE);
    assert(target.relax && target.relax_context);
    assert(primary_display->ops->relax(primary_display, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(relax_driver == primary_display->driver);
    assert(solar_os_ssd1685_detach("display0") == ESP_OK);
    puts("ssd1685_expansion_test: PASS"); return 0;
}
