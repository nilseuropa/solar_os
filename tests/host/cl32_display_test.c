#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/services/solar_os_cl32_display.c"

static cl32_display_controller_t actual;
static esp_err_t identify_error, attach_error, detach_error;
static unsigned identifies, attaches;
static bool used_epd;

size_t strlcpy(char *dst, const char *src, size_t size)
{ const size_t n = strlen(src); if (size) snprintf(dst, size, "%s", src); return n; }
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{ (void)index; if (bus) bus->mosi_pin = 10; return !strcmp(name, "spi0"); }
bool solar_os_expansion_spi_cs_allowed(const char *bus, int pin)
{ return !strcmp(bus, "spi0") && pin == 6; }
esp_err_t cl32_display_identify(const char *bus, int cs, int dc, int reset, int mosi,
                               cl32_display_controller_t *result)
{
    assert(!strcmp(bus, "spi0") && cs == 6 && dc == 13 && reset == 12 && mosi == 10);
    identifies++; *result = actual; return identify_error;
}
static esp_err_t child_attach(const char *name, const solar_os_expansion_binding_t *bindings,
                              size_t count, bool epd)
{
    assert(!strcmp(name, "display0") && count == (epd ? 7U : 6U));
    assert(!strcmp(bindings[count - 2].role, "panel") && bindings[count - 2].value == 1);
    assert(!strcmp(bindings[count - 1].role, "rotation") && bindings[count - 1].value == 3);
    bool busy_found = false;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(bindings[i].role, "busy")) { busy_found = true; assert(bindings[i].value == 14); }
    }
    assert(busy_found == epd);
    attaches++; used_epd = epd; return attach_error;
}
esp_err_t solar_os_ssd1685_attach(const char *n, const solar_os_expansion_binding_t *b, size_t c)
{ return child_attach(n, b, c, true); }
static esp_err_t rlcd_attach(const char *n, const solar_os_expansion_binding_t *b, size_t c)
{ return child_attach(n, b, c, false); }
esp_err_t solar_os_ssd1685_detach(const char *name)
{ assert(used_epd && !strcmp(name, "display0")); return detach_error; }
static esp_err_t rlcd_detach(const char *name)
{ assert(!used_epd && !strcmp(name, "display0")); return detach_error; }
const solar_os_expansion_driver_t solar_os_st7305_expansion_driver = {
    .attach = rlcd_attach, .detach = rlcd_detach,
};

int main(void)
{
    assert(solar_os_cl32_display_expansion_driver.board_only);
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target = "spi0"},
        {.kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value = 6},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .value = 13},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .value = 12},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .value = 14},
    };
    assert(attach("display0", bindings, 4) == ESP_ERR_INVALID_ARG && !identifies);
    identify_error = ESP_ERR_NOT_SUPPORTED;
    assert(attach("display0", bindings, 5) == ESP_ERR_NOT_SUPPORTED && !attaches);
    identify_error = ESP_OK; actual = CL32_DISPLAY_SSD1685; attach_error = ESP_FAIL;
    assert(attach("display0", bindings, 5) == ESP_FAIL && !device_name[0]);
    attach_error = ESP_OK;
    assert(attach("display0", bindings, 5) == ESP_OK && used_epd);
    assert(attach("display0", bindings, 5) == ESP_ERR_INVALID_STATE);
    assert(detach("other") == ESP_ERR_NOT_FOUND);
    detach_error = ESP_FAIL;
    assert(detach("display0") == ESP_FAIL && device_name[0]);
    detach_error = ESP_OK;
    assert(detach("display0") == ESP_OK && !device_name[0]);
    actual = CL32_DISPLAY_ST7305;
    assert(attach("display0", bindings, 5) == ESP_OK && !used_epd && identifies == 4);
    assert(detach("display0") == ESP_OK);
    actual = CL32_DISPLAY_UNKNOWN;
    assert(attach("display0", bindings, 5) == ESP_ERR_NOT_SUPPORTED);
    puts("CL-32 board display adapter tests passed");
}
