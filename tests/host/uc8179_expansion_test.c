#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/services/solar_os_uc8179.c"
#include "../../src/services/solar_os_uc8179_driver.c"

static int initialized, deinited;
static esp_err_t init_error, register_error, unregister_error;
static bool primary;
static solar_os_display_target_t target;
static solar_os_board_display_t *primary_display;
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
esp_err_t epd_uc8179_init(epd_ultrachip_t *display, const epd_ultrachip_config_t *config)
{
    initialized++;
    assert(config->panel == 1 && config->spi_clock_hz == 16000000);
    assert(config->cs_pin == 10 && config->dc_pin == 17 && config->busy_pin == 15);
    assert(config->power_pin == -1 && config->rotation == U8G2_R0);
    display->u8g2.width = 800; display->u8g2.height = 480;
    return init_error;
}
void epd_ultrachip_deinit(epd_ultrachip_t *display) { (void)display; deinited++; }
esp_err_t epd_ultrachip_resume(epd_ultrachip_t *display) { (void)display; return ESP_OK; }
u8g2_t *epd_ultrachip_get_u8g2(epd_ultrachip_t *display) { return &display->u8g2; }
const char *epd_ultrachip_controller_mode(const epd_ultrachip_t *display)
{ (void)display; return "refresh=auto"; }
const char *epd_ultrachip_controller_mode_values(const epd_ultrachip_t *display)
{ (void)display; return "refresh=<auto,partial,full>"; }
esp_err_t epd_ultrachip_set_controller_mode(epd_ultrachip_t *display, const char *mode)
{ (void)display; (void)mode; return ESP_OK; }

static void *relax_driver;
esp_err_t epd_ultrachip_relax(epd_ultrachip_t *d, solar_os_display_relax_phase_t phase)
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
    assert(solar_os_uc8179_attach("epd0", bindings, 5) == ESP_ERR_INVALID_ARG);
    assert(initialized == 0); /* no implicit panel selection */
    bindings[5].value = 0;
    assert(solar_os_uc8179_attach("epd0", bindings, 6) == ESP_ERR_INVALID_ARG);
    bindings[5].value = 1;
    init_error = ESP_FAIL;
    assert(solar_os_uc8179_attach("epd0", bindings, 6) == ESP_FAIL);
    assert(deinited == 1);
    init_error = ESP_OK; register_error = ESP_FAIL;
    assert(solar_os_uc8179_attach("epd0", bindings, 6) == ESP_FAIL);
    assert(deinited == 2);
    register_error = ESP_OK;
    assert(solar_os_uc8179_attach("epd0", bindings, 6) == ESP_OK);
    assert(target.width == 800 && target.height == 480 && !target.black_is_one);
    assert(strcmp(target.driver, "uc8179") == 0 && strcmp(target.controller, "UC8179") == 0);
    assert(solar_os_uc8179_attach("epd0", bindings, 6) == ESP_ERR_INVALID_ARG);
    assert(target.relax(target.relax_context, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(relax_driver == target.relax_context);
    unregister_error = ESP_FAIL;
    assert(solar_os_uc8179_detach("epd0") == ESP_FAIL && deinited == 2);
    unregister_error = ESP_OK;
    assert(solar_os_uc8179_detach("epd0") == ESP_OK && deinited == 3);
    assert(solar_os_uc8179_detach("epd0") == ESP_ERR_NOT_FOUND);
    primary = true;
    assert(solar_os_uc8179_attach("display0", bindings, 6) == ESP_OK);
    assert(primary_display->ready && strcmp(primary_display->controller, "UC8179") == 0);
    assert(target.relax && target.relax_context);
    assert(primary_display->ops->relax(primary_display, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(relax_driver == primary_display->driver);
    assert(solar_os_uc8179_detach("display0") == ESP_OK);
    puts("uc8179_expansion_test: PASS"); return 0;
}
