#include <assert.h>
#include <stdio.h>
#include "../../src/services/solar_os_sdmmc.c"
#include "../../src/services/solar_os_sdmmc_driver.c"

static int configured, power_configured, cleared, mounted;
static bool built_in = true, has_mounts;
static esp_err_t power_error, mount_error;
size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len=strlen(src); if (size) { size_t n=len<size-1?len:size-1; memcpy(dst,src,n); dst[n]=0; } return len; }
bool solar_os_board_has(solar_os_board_capabilities_t cap)
{ assert(cap==SOLAR_OS_BOARD_CAP_SD); return built_in; }
esp_err_t sd_card_configure_sdmmc(int clk, int cmd, int d0, int d1, int d2, int d3)
{ assert(clk==41 && cmd==42 && d0==40 && d1==-1 && d2==-1 && d3==-1); configured++; return ESP_OK; }
esp_err_t sd_card_configure_sdmmc_power(int pin, int active)
{ assert(pin==5 && active==0 && configured); power_configured++; return power_error; }
esp_err_t sd_card_clear_sdmmc_config(void) { cleared++; return ESP_OK; }
esp_err_t sd_card_init(void) { mounted++; return mount_error; }
esp_err_t solar_os_storage_mount(void) { mounted++; return mount_error; }
esp_err_t sd_card_unmount(void) { return ESP_OK; }
bool sd_card_has_mounts(void) { return has_mounts; }

int main(void)
{
    solar_os_expansion_binding_t b[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="clk", .value=41},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="cmd", .value=42},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="d0", .value=40},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="power", .value=5},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role="active", .value=0},
    };
    assert(solar_os_sdmmc_attach("storage0",b,5)==ESP_OK && power_configured==1 && mounted==0);
    assert(solar_os_sdmmc_attach("other",b,5)==ESP_ERR_INVALID_ARG);
    mount_error=ESP_ERR_TIMEOUT;
    assert(solar_os_sdmmc_attach("storage0",b,5)==ESP_ERR_TIMEOUT && configured==1);
    mount_error=ESP_OK;
    assert(solar_os_sdmmc_attach("storage0",b,5)==ESP_OK && configured==1);
    mounted=0;
    has_mounts=true; assert(solar_os_sdmmc_detach("storage0")==ESP_ERR_INVALID_STATE);
    has_mounts=false; assert(solar_os_sdmmc_detach("storage0")==ESP_OK && cleared==1);
    power_error=ESP_FAIL;
    assert(solar_os_sdmmc_attach("storage0",b,5)==ESP_FAIL && cleared==2 && !sdmmc.active);
    power_error=ESP_OK; built_in=false; mount_error=ESP_ERR_TIMEOUT;
    assert(solar_os_sdmmc_attach("storage0",b,5)==ESP_ERR_TIMEOUT && cleared==3 && mounted==1);
    mount_error=ESP_OK;
    assert(solar_os_sdmmc_attach("storage0",b,3)==ESP_OK && power_configured==3 && mounted==2);
    assert(solar_os_sdmmc_detach("storage0")==ESP_OK);
    b[3]=b[4];
    assert(solar_os_sdmmc_attach("storage0",b,4)==ESP_ERR_INVALID_ARG);
    puts("SDMMC power-binding and rollback tests passed");
}
