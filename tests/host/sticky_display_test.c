#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/services/solar_os_sticky_display.c"

static epd_sticky_controller_t detected;
static esp_err_t identify_error, attach_error, detach_error;
static unsigned identifies, attaches;
static bool used_ssd2677;

size_t strlcpy(char *out, const char *in, size_t size)
{ const size_t len = strlen(in); if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(out,in,n); out[n] = 0; } return len; }
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{ (void)bus; (void)index; return !strcmp(name,"spi0"); }
bool solar_os_expansion_spi_cs_allowed(const char *bus, int pin) { return !strcmp(bus,"spi0") && pin == 15; }
esp_err_t epd_sticky_identify(const char *bus,int cs,int dc,int reset,int busy,int power,epd_sticky_controller_t *c)
{
    assert(!strcmp(bus,"spi0") && cs == 15 && dc == 16 && reset == 17 && busy == 18 && power == 47);
    identifies++; *c = detected; return identify_error;
}
static esp_err_t child_attach(const char *name,const solar_os_expansion_binding_t *bindings,size_t count,bool v2)
{
    assert(!strcmp(name,"display0") && count == 9);
    assert(!strcmp(bindings[count - 1].role,"panel") && bindings[count - 1].value == 1);
    attaches++; used_ssd2677 = v2; return attach_error;
}
esp_err_t solar_os_ssd1677_attach(const char *n,const solar_os_expansion_binding_t *b,size_t count)
{ return child_attach(n,b,count,false); }
esp_err_t solar_os_ssd2677_attach(const char *n,const solar_os_expansion_binding_t *b,size_t count)
{ return child_attach(n,b,count,true); }
esp_err_t solar_os_ssd1677_detach(const char *n) { assert(!used_ssd2677 && !strcmp(n,"display0")); return detach_error; }
esp_err_t solar_os_ssd2677_detach(const char *n) { assert(used_ssd2677 && !strcmp(n,"display0")); return detach_error; }

int main(void)
{
    assert(solar_os_sticky_display_expansion_driver.board_only);
    solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS,.target="spi0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS,.value=15},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="dc",.value=16},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="reset",.value=17},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="busy",.value=18},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="power",.value=47},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="clock",.value=20000},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="rotation",.value=0},
    };
    bindings[6].value = 20001;
    assert(attach("display0",bindings,8) == ESP_ERR_INVALID_ARG && !identifies);
    bindings[6].value = 20000; identify_error = ESP_ERR_TIMEOUT;
    assert(attach("display0",bindings,8) == ESP_ERR_TIMEOUT && !attaches);
    identify_error = ESP_OK; detected = EPD_STICKY_SSD2677; attach_error = ESP_FAIL;
    assert(attach("display0",bindings,8) == ESP_FAIL && !device_name[0]);
    attach_error = ESP_OK;
    assert(attach("display0",bindings,8) == ESP_OK && used_ssd2677);
    assert(attach("display0",bindings,8) == ESP_ERR_INVALID_ARG);
    assert(detach("other") == ESP_ERR_NOT_FOUND);
    detach_error = ESP_FAIL;
    assert(detach("display0") == ESP_FAIL && device_name[0]);
    detach_error = ESP_OK;
    assert(detach("display0") == ESP_OK && !device_name[0]);
    detected = EPD_STICKY_SSD1677;
    assert(attach("display0",bindings,8) == ESP_OK && !used_ssd2677);
    assert(detach("display0") == ESP_OK);
    puts("sticky_display_test: PASS");
    return 0;
}
