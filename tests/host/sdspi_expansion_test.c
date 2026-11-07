#include <assert.h>
#include <stdio.h>
#include "../../src/services/solar_os_sdspi.c"
#include "../../src/services/solar_os_sdspi_driver.c"

static int configured, cleared, mounted, power_on, power_off;
static bool built_in = true, has_mounts;
static esp_err_t mount_error;
size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len=strlen(src); if (size) { size_t n=len<size-1?len:size-1; memcpy(dst,src,n); dst[n]=0; } return len; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
bool solar_os_board_has(solar_os_board_capabilities_t cap)
{ assert(cap==SOLAR_OS_BOARD_CAP_SD); return built_in; }
bool solar_os_expansion_spi_cs_allowed(const char *bus, int pin)
{ return strcmp(bus,"spi0")==0 && pin==39; }
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{ (void)index; memset(bus,0,sizeof(*bus)); bus->host=2; return strcmp(name,"spi0")==0; }
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *line, bool level)
{ assert(strcmp(line->controller,"gpiox0")==0 && line->line==12); if (level) power_on++; else power_off++; return ESP_OK; }
esp_err_t sd_card_configure_sdspi(int host, int cs)
{ assert(host==2 && cs==39); configured++; return ESP_OK; }
esp_err_t sd_card_clear_sdspi_config(void) { cleared++; return ESP_OK; }
esp_err_t sd_card_init(void) { mounted++; return mount_error; }
esp_err_t solar_os_storage_mount(void) { mounted++; return mount_error; }
esp_err_t sd_card_unmount(void) { return ESP_OK; }
bool sd_card_has_mounts(void) { return has_mounts; }
bool sd_card_get_last_diagnostics(sd_card_diagnostics_t *d) { (void)d; return false; }

int main(void)
{
    solar_os_expansion_binding_t b[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target="spi0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value=39},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role="power", .target="gpiox0", .value=12},
    };
    assert(solar_os_sdspi_expansion_driver.early);

    /* Built-in card: attach powers the line and configures SPI but leaves
     * the mount to storage init, even if the card would fail to mount. */
    mount_error=ESP_ERR_TIMEOUT;
    assert(solar_os_sdspi_attach("storage0",b,3)==ESP_OK);
    assert(configured==1 && mounted==0 && power_on==1 && power_off==0 && sdspi.active);
    assert(solar_os_sdspi_attach("other",b,3)==ESP_ERR_INVALID_ARG);
    /* Attaching it again by name retries the mount. */
    assert(solar_os_sdspi_attach("storage0",b,3)==ESP_ERR_TIMEOUT && mounted==1);
    mount_error=ESP_OK;
    has_mounts=true; assert(solar_os_sdspi_detach("storage0")==ESP_ERR_INVALID_STATE);
    has_mounts=false; assert(solar_os_sdspi_detach("storage0")==ESP_OK && cleared==1 && !sdspi.active);

    /* Runtime card on a board without built-in SD still mounts on attach,
     * and a failed mount rolls back. */
    built_in=false; mounted=0; mount_error=ESP_ERR_TIMEOUT;
    assert(solar_os_sdspi_attach("storage0",b,3)==ESP_ERR_TIMEOUT);
    assert(mounted==1 && cleared==2 && power_off==power_on && !sdspi.active);
    mount_error=ESP_OK;
    assert(solar_os_sdspi_attach("storage0",b,2)==ESP_OK && mounted==2 && sdspi.active);
    assert(solar_os_sdspi_detach("storage0")==ESP_OK);
    puts("SDSPI built-in and runtime attach tests passed");
}
