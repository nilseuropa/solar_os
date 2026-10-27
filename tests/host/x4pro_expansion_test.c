#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/services/solar_os_x4pro.c"
#include "../../src/services/solar_os_x4pro_driver.c"

const u8g2_cb_t u8g2_cb_r0 = {0};
static bool cached, latch_on, latch_held, deep_hold;
static uint8_t cached_id;
static int probes, writes, commits, initializes;
static epd_xteink_controller_t detected = EPD_XTEINK_UC8179;
static esp_err_t probe_error, init_error, commit_error, register_error;
static solar_os_board_display_t *registered;

size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t len = strlen(src); if (size) { size_t n = len < size-1 ? len : size-1; memcpy(dst, src, n); dst[n]=0; } return len; }
esp_err_t gpio_config(const gpio_config_t *c)
{ assert(c->pin_bit_mask == 2 && c->mode == GPIO_MODE_OUTPUT); return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{ assert(pin == 1 && level <= 1 && !latch_held); latch_on = level != 0; return ESP_OK; }
esp_err_t gpio_hold_en(gpio_num_t pin)
{ assert(pin == 1 && !latch_on); latch_held = true; return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t pin)
{ assert(pin == 1); latch_held = false; return ESP_OK; }
void gpio_deep_sleep_hold_en(void) { deep_hold = true; }
bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{ (void)index; if (strcmp(name,"spi0")) return false; bus->mosi_pin=11; return true; }
bool solar_os_expansion_spi_cs_allowed(const char *name, int pin)
{ return !strcmp(name,"spi0") && pin == 13; }
esp_err_t epd_xteink_identify(const char *bus, int cs, int dc, int reset, int mosi,
                            epd_xteink_controller_t *id)
{ assert(latch_on && !strcmp(bus,"spi0") && cs==13 && dc==18 && reset==14 && mosi==11); probes++; *id=detected; return probe_error; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ assert(!strcmp(name,"x4pro")); *handle=1; return mode==NVS_READONLY && !cached ? ESP_ERR_NVS_NOT_FOUND : ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ assert(handle==1 && !strcmp(key,"controller")); *value=cached_id; return cached ? ESP_OK : ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ assert(handle==1 && !strcmp(key,"controller") && initializes>0 && registered); writes++; cached_id=value; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ assert(handle==1); commits++; if (!commit_error) cached=true; return commit_error; }
void nvs_close(nvs_handle_t handle) { assert(handle==1); }
esp_err_t frontlight_pwm_init(frontlight_pwm_t *light, int cool, int warm)
{ assert(latch_on && cool==8 && warm==9); light->ready=true; return ESP_OK; }
esp_err_t frontlight_pwm_set(frontlight_pwm_t *light, uint8_t percent)
{ light->percent=percent; return ESP_OK; }
esp_err_t frontlight_pwm_suspend(frontlight_pwm_t *light, bool suspended)
{ light->suspended=suspended; return ESP_OK; }
void frontlight_pwm_deinit(frontlight_pwm_t *light) { light->ready=false; }
esp_err_t epd_ssd1677_init(epd_ssd1677_t *d, const epd_ssd1677_config_t *c)
{ (void)d; assert(c->spi_clock_hz==10000000 && c->rotation==U8G2_R0 && c->power_pin==-1 && c->busy_level==1); initializes++; return init_error; }
static esp_err_t init_uc(const epd_ultrachip_config_t *c)
{ assert(c->spi_clock_hz==10000000 && c->panel==1 && c->rotation==U8G2_R0 && c->power_pin==-1); initializes++; return init_error; }
esp_err_t epd_uc8179_init(epd_ultrachip_t *d, const epd_ultrachip_config_t *c)
{ (void)d; assert(detected==EPD_XTEINK_UC8179 || cached); return init_uc(c); }
esp_err_t epd_uc8279_init(epd_ultrachip_t *d, const epd_ultrachip_config_t *c)
{ (void)d; assert(detected==EPD_XTEINK_UC8279 || cached); return init_uc(c); }
u8g2_t *epd_ssd1677_get_u8g2(epd_ssd1677_t *d) { return &d->u8g2; }
u8g2_t *epd_ultrachip_get_u8g2(epd_ultrachip_t *d) { return &d->u8g2; }
void epd_ssd1677_deinit(epd_ssd1677_t *d) { (void)d; }
void epd_ultrachip_deinit(epd_ultrachip_t *d) { (void)d; }
esp_err_t epd_ssd1677_resume(epd_ssd1677_t *d) { (void)d; return ESP_OK; }
esp_err_t epd_ultrachip_resume(epd_ultrachip_t *d) { (void)d; return ESP_OK; }
const char *epd_ssd1677_controller_mode(const epd_ssd1677_t *d) { (void)d; return "refresh=auto"; }
const char *epd_ultrachip_controller_mode(const epd_ultrachip_t *d) { (void)d; return "refresh=auto"; }
const char *epd_ssd1677_controller_mode_values(const epd_ssd1677_t *d) { (void)d; return "refresh=auto|partial|full"; }
const char *epd_ultrachip_controller_mode_values(const epd_ultrachip_t *d) { (void)d; return "refresh=auto|partial|full"; }
esp_err_t epd_ssd1677_set_controller_mode(epd_ssd1677_t *d, const char *mode) { (void)d; (void)mode; return ESP_OK; }
esp_err_t epd_ultrachip_set_controller_mode(epd_ultrachip_t *d, const char *mode) { (void)d; (void)mode; return ESP_OK; }
void u8x8_SetPowerSave(u8x8_t *u8x8, uint8_t enabled) { assert(u8x8 && enabled<=1); }
esp_err_t solar_os_board_display_register_primary(solar_os_board_display_t *d)
{ assert(d->width==800 && d->height==480 && d->ready && d->u8g2); if (!register_error) registered=d; return register_error; }

static const solar_os_expansion_binding_t bindings[] = {
    {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .target="spi0"},
    {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS, .value=13},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="dc", .value=18},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="reset", .value=14},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="busy", .value=6},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="latch", .value=1},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="cool", .value=8},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="warm", .value=9},
};
static esp_err_t attach(void) { return solar_os_x4pro_attach("display0", bindings, 8); }
static void reboot(void)
{
    if (device) { deinit(&device->display); heap_caps_free(device); device=NULL; }
    registered=NULL; latch_on=false; probes=writes=commits=initializes=0;
    probe_error=init_error=commit_error=register_error=ESP_OK;
}
static bool relaxed_ssd;
esp_err_t epd_ssd1677_relax(epd_ssd1677_t *d, solar_os_display_relax_phase_t phase)
{ assert(d && phase == SOLAR_OS_DISPLAY_RELAX_BEGIN); relaxed_ssd = true; return ESP_OK; }
esp_err_t epd_ultrachip_relax(epd_ultrachip_t *d, solar_os_display_relax_phase_t phase)
{ assert(d && phase == SOLAR_OS_DISPLAY_RELAX_BEGIN); relaxed_ssd = false; return ESP_OK; }
int main(void)
{
    assert(attach()==ESP_OK && probes==1 && commits==1 && cached_id==EPD_XTEINK_UC8179);
    assert(registered->ops->relax(registered, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(!relaxed_ssd);
    assert(registered->ops->brightness_supported(registered));
    assert(registered->ops->set_brightness(registered, 42)==ESP_OK);
    uint8_t brightness=0;
    assert(registered->ops->get_brightness(registered, &brightness)==ESP_OK && brightness==42);
    assert(registered->ops->set_power_save(registered, true)==ESP_OK && device->light.suspended);
    assert(registered->ops->set_power_save(registered, false)==ESP_OK && !device->light.suspended);
    assert(device->light.percent==42);
    assert(registered->ops->shutdown(registered)==ESP_OK && !latch_on && latch_held && deep_hold);
    reboot(); detected=EPD_XTEINK_UC8279; probe_error=ESP_FAIL;
    assert(attach()==ESP_OK && probes==0 && commits==0 && !strcmp(registered->controller,"UC8179"));
    assert(latch_on && !latch_held);
    reboot(); cached=false;
    assert(attach()==ESP_OK && commits==1 && cached_id==EPD_XTEINK_UC8279);
    reboot(); cached=false; detected=EPD_XTEINK_SSD1677;
    assert(attach()==ESP_OK && commits==1 && !strcmp(registered->controller,"SSD1677"));
    assert(registered->ops->relax(registered, SOLAR_OS_DISPLAY_RELAX_BEGIN) == ESP_OK);
    assert(relaxed_ssd);
    reboot(); cached=false; probe_error=ESP_ERR_NOT_SUPPORTED;
    assert(attach()==ESP_ERR_NOT_SUPPORTED && !commits && !device);
    reboot(); cached=false; init_error=ESP_ERR_TIMEOUT;
    assert(attach()==ESP_ERR_TIMEOUT && !writes && !device);
    reboot(); cached=false; register_error=ESP_ERR_INVALID_STATE;
    assert(attach()==ESP_ERR_INVALID_STATE && !writes && !device);
    reboot(); cached=false; commit_error=ESP_FAIL;
    assert(attach()==ESP_OK && !cached && commits==1);
    reboot(); assert(attach()==ESP_OK && probes==1);
    reboot(); cached=true; cached_id=99;
    assert(attach()==ESP_ERR_INVALID_STATE && !probes && !writes && !device);
    reboot();
    puts("X4 Pro NVS and display-provider tests passed");
}
