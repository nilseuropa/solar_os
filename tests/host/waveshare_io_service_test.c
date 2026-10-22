#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_waveshare_io.h"
#include "solar_os_backlight.h"
#include "solar_os_battery.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_buses.h"

static uint8_t regs[8];
static solar_os_gpio_controller_registration_t gpio;
static solar_os_battery_provider_t battery;
static solar_os_backlight_set_fn_t dimmer;
static void *dimmer_user;
static bool busy;
static esp_err_t register_error;
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *b, size_t *i)
{ (void)b; (void)i; return !strcmp(name, "i2c0"); }
esp_err_t solar_os_bus_i2c_probe(const char *b, uint8_t a)
{ assert(!strcmp(b,"i2c0") && a==0x24); return ESP_OK; }
esp_err_t solar_os_bus_i2c_read_reg(const char *b, uint8_t a, uint8_t r, uint8_t *p, size_t n)
{ (void)b; (void)a; assert(r+n<=sizeof(regs)); memcpy(p,regs+r,n); return ESP_OK; }
esp_err_t solar_os_bus_i2c_write_reg(const char *b, uint8_t a, uint8_t r, const uint8_t *p, size_t n)
{ (void)b; (void)a; assert(r+n<=sizeof(regs)); memcpy(regs+r,p,n); return ESP_OK; }
esp_err_t solar_os_gpio_controller_register(const solar_os_gpio_controller_registration_t *r)
{ assert(!gpio.name); gpio=*r; return ESP_OK; }
esp_err_t solar_os_gpio_controller_unregister(const char *n)
{ assert(!strcmp(gpio.name,n)); if(busy) return ESP_ERR_INVALID_STATE; memset(&gpio,0,sizeof(gpio)); return ESP_OK; }
esp_err_t solar_os_battery_register_provider(const char *n, const solar_os_battery_provider_t *p)
{ (void)n; if(register_error) return register_error; battery=*p; return ESP_OK; }
esp_err_t solar_os_battery_unregister_provider(const char *n)
{ (void)n; memset(&battery,0,sizeof(battery)); return ESP_OK; }
esp_err_t solar_os_backlight_register(solar_os_backlight_set_fn_t f, void *u)
{ dimmer=f; dimmer_user=u; return f(u,100); }
void solar_os_backlight_unregister(void *u)
{ assert(u==dimmer_user); dimmer=NULL; dimmer_user=NULL; }

int main(void)
{
    const solar_os_expansion_binding_t bindings[]={
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS,.target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,.value=0x24},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="battery",.value=1},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="backlight",.value=1},
    };
    register_error=ESP_ERR_INVALID_STATE;
    assert(solar_os_waveshare_io_attach("io0",bindings,4)==register_error);
    assert(!gpio.name && !battery.read && !dimmer);
    register_error=ESP_OK;
    assert(solar_os_waveshare_io_attach("io0",bindings,4)==ESP_OK);
    assert(gpio.line_count==8 && regs[2]==0 && regs[3]==255);
    assert(dimmer(dimmer_user,50)==ESP_OK && regs[5]==128);
    assert(dimmer(dimmer_user,0)==ESP_OK && regs[5]==255);
    assert(gpio.ops->write(gpio.ctx,2,false)==ESP_OK && regs[3]==251 && regs[2]==4);
    regs[6]=0xa5; regs[7]=1; /* 421 of 1023 => 4074 mV, nominal. */
    solar_os_battery_sample_t sample;
    assert(battery.read(battery.user,&sample)==ESP_OK);
    assert(sample.battery_mv==4074 && !sample.calibrated && !sample.charging_valid);
    busy=true;
    assert(solar_os_waveshare_io_detach("io0")==ESP_ERR_INVALID_STATE);
    assert(gpio.name && battery.read && dimmer);
    busy=false;
    assert(solar_os_waveshare_io_detach("io0")==ESP_OK);
    assert(!gpio.name && !battery.read && !dimmer);
    puts("Waveshare I/O providers, nominal battery, dimmer, busy detach and rollback: ok");
}
