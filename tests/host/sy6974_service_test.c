#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_sy6974.h"
#include "solar_os_charger.h"
#include "solar_os_battery.h"
#include "solar_os_buses.h"
#include "adc_port.h"
#include "freertos/task.h"

static uint8_t regs[12];
static bool charger_registered, battery_registered;
static solar_os_charger_registration_t charger;
static solar_os_battery_provider_t battery;
static unsigned power;
static esp_err_t adc_error, charger_error, battery_error;
static bool calibrated = true;
size_t strlcpy(char *dst,const char *src,size_t n)
{ size_t len=strlen(src); if(n) { size_t copy=len<n-1?len:n-1; memcpy(dst,src,copy); dst[copy]=0; } return len; }
bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus,size_t *index)
{ (void)bus; (void)index; return !strcmp(name,"i2c0"); }
esp_err_t solar_os_bus_i2c_probe(const char *name,uint8_t addr)
{ assert(!strcmp(name,"i2c0") && addr==0x6b); return ESP_OK; }
esp_err_t solar_os_bus_i2c_read_reg(const char *name,uint8_t addr,uint8_t reg,uint8_t *out,size_t size)
{ assert(!strcmp(name,"i2c0") && addr==0x6b && reg<12 && size==1); *out=regs[reg]; return ESP_OK; }
esp_err_t solar_os_bus_i2c_write_reg(const char *name,uint8_t addr,uint8_t reg,const uint8_t *in,size_t size)
{ assert(!strcmp(name,"i2c0") && addr==0x6b && reg<12 && size==1); regs[reg]=*in; return ESP_OK; }
esp_err_t adc_port_configure_pin(gpio_num_t pin,adc_atten_t atten,adc_bitwidth_t bits)
{ assert(pin==1 && atten==12 && bits==12); return ESP_OK; }
esp_err_t adc_port_read(gpio_num_t pin,adc_port_sample_t *out)
{ assert(pin==1 && power==1); *out=(adc_port_sample_t){.voltage_mv=2000,.calibrated=calibrated}; return adc_error; }
esp_err_t gpio_set_level(gpio_num_t pin,uint32_t level) { assert(pin==40); power=level; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin,gpio_mode_t mode)
{ assert(pin==40 && mode==GPIO_MODE_OUTPUT && power==0); return ESP_OK; }
void vTaskDelay(TickType_t ticks) { assert(ticks==pdMS_TO_TICKS(10) && power==1); }
esp_err_t solar_os_charger_register(const solar_os_charger_registration_t *reg)
{ if(charger_error) return charger_error; assert(!charger_registered); charger=*reg; charger_registered=true; return ESP_OK; }
esp_err_t solar_os_charger_unregister(const char *name)
{ assert(!strcmp(name,"charger0") && charger_registered); charger_registered=false; return ESP_OK; }
esp_err_t solar_os_battery_register_provider(const char *name,const solar_os_battery_provider_t *p)
{ assert(!strcmp(name,"charger0")); if(battery_error) return battery_error; assert(!battery_registered); battery=*p; battery_registered=true; return ESP_OK; }
esp_err_t solar_os_battery_unregister_provider(const char *name)
{ assert(!strcmp(name,"charger0") && battery_registered); battery_registered=false; return ESP_OK; }

int main(void)
{
    solar_os_expansion_binding_t bindings[] = {
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS,.target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,.value=0x6b},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="input_current",.value=1000},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="charge_current",.value=480},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="charge_voltage",.value=4208},
        {.kind=SOLAR_OS_EXPANSION_BINDING_ADC,.value=1},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="divider",.value=2000},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="power",.value=40},
    };
    bindings[3].value=66016; /* must not truncate to a valid 16-bit current */
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_ERR_INVALID_ARG);
    bindings[3].value=480;
    regs[11]=0x40; regs[1]=0x10; regs[8]=0x54;
    battery_error=ESP_ERR_INVALID_STATE;
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_ERR_INVALID_STATE);
    assert(!charger_registered && !battery_registered && !power);
    battery_error=0; charger_error=ESP_FAIL;
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_FAIL);
    assert(!charger_registered && !battery_registered && !power);
    charger_error=0;
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_OK);
    assert(charger_registered && battery_registered && !power);
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_ERR_INVALID_STATE);
    solar_os_battery_sample_t sample;
    assert(battery.read(battery.user,&sample)==ESP_OK);
    assert(sample.battery_mv==4000 && sample.calibrated && sample.charging_valid && sample.charging);
    assert(sample.external_power_valid && sample.external_power && !power);
    solar_os_charger_status_t status;
    assert(charger.ops->read_status(charger.ctx,&status)==ESP_OK);
    assert(status.charge_current_ma==480 && status.charge_voltage_mv==4208 && status.input_current_limit_ma==1000);
    regs[8]=0;
    assert(battery.read(battery.user,&sample)==ESP_OK && !sample.charging && !sample.external_power);
    calibrated=false;
    assert(battery.read(battery.user,&sample)==ESP_ERR_NOT_SUPPORTED && !power);
    calibrated=true; adc_error=ESP_ERR_TIMEOUT;
    assert(battery.read(battery.user,&sample)==ESP_ERR_TIMEOUT && !power);
    adc_error=0;
    solar_os_battery_provider_t stale=battery;
    assert(solar_os_sy6974_detach("charger0")==ESP_OK);
    assert(!charger_registered && !battery_registered && !power);
    assert(stale.read(stale.user,&sample)==ESP_ERR_INVALID_STATE);
    assert(solar_os_sy6974_attach("charger0",bindings,8)==ESP_OK);
    assert(solar_os_sy6974_detach("charger0")==ESP_OK);
    puts("SY6974B charger/battery provider, power gating and rollback tests passed");
    return 0;
}
