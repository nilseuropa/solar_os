#include <assert.h>
#include <string.h>
#include "../../src/services/solar_os_mpu6050.c"

static uint8_t regs[256];
static int64_t now;
static solar_os_imu_registration_t provider;
static esp_err_t register_result,unregister_result;
static unsigned reads, gpio_configs, released;
static int irq_level, fail_write=-1;

size_t strlcpy(char *dst,const char *src,size_t size)
{ size_t n=strlen(src);if(size){size_t copy=n<size-1?n:size-1;memcpy(dst,src,copy);dst[copy]=0;}return n; }
void vSemaphoreDelete(SemaphoreHandle_t mutex) { assert(mutex);++released; }
void vTaskDelay(TickType_t ticks) { now+=(int64_t)ticks*1000; }
int64_t esp_timer_get_time(void) { return now; }
esp_err_t gpio_config(const gpio_config_t *config)
{ assert(config->pin_bit_mask==(1ULL<<7) && config->mode==GPIO_MODE_INPUT);++gpio_configs;return ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin==7);return irq_level; }
bool solar_os_expansion_find_i2c_bus(const char *name,solar_os_expansion_i2c_bus_t *bus,size_t *index)
{ (void)bus;(void)index;return !strcmp(name,"i2c0"); }
esp_err_t solar_os_bus_i2c_read_reg(const char *bus,uint8_t addr,uint8_t reg,uint8_t *data,size_t len)
{
    assert(!strcmp(bus,"i2c0") && (addr==0x68 || addr==0x69));++reads;
    if(reg==0x6b) regs[reg]&=~0x80;
    memcpy(data,regs+reg,len);
    if(reg==0x3a) {regs[reg]=0;irq_level=0;}
    return ESP_OK;
}
esp_err_t solar_os_bus_i2c_write_reg(const char *bus,uint8_t addr,uint8_t reg,const uint8_t *data,size_t len)
{
    assert(!strcmp(bus,"i2c0") && (addr==0x68 || addr==0x69) && len==1);
    if(reg==fail_write)return ESP_FAIL;
    regs[reg]=*data;return ESP_OK;
}
esp_err_t solar_os_imu_register(const solar_os_imu_registration_t *r)
{ assert(!strcmp(r->driver,"mpu6050") && r->capabilities==3);provider=*r;return register_result; }
esp_err_t solar_os_imu_unregister(const char *name)
{ assert(!strcmp(name,"imu0"));return unregister_result; }

int main(void)
{
    solar_os_expansion_binding_t b[]={
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS,.target="i2c0"},
        {.kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,.value=0x68},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="irq",.value=7}};
    regs[0x75]=0x68;
    assert(solar_os_mpu6050_attach("imu0",b,1)==ESP_ERR_INVALID_ARG);
    b[1].value=0x67;assert(solar_os_mpu6050_attach("imu0",b,2)==ESP_ERR_INVALID_ARG);b[1].value=0x68;
    register_result=ESP_FAIL;
    assert(solar_os_mpu6050_attach("imu0",b,2)==ESP_FAIL && released==1 && regs[0x6b]==0x40);
    register_result=ESP_OK;
    assert(solar_os_mpu6050_attach("imu0",b,2)==ESP_OK && !gpio_configs);
    assert(solar_os_mpu6050_attach("imu0",b,2)==ESP_ERR_INVALID_STATE);
    solar_os_imu_sample_t sample;unsigned before=reads;int64_t start=now;
    assert(provider.ops->read_sample(provider.ctx,3,&sample)==ESP_ERR_TIMEOUT && now-start==3000 && reads>before);
    regs[0x3a]=1;regs[0x3f]=0x40; /* +1 g on Z. */
    assert(provider.ops->read_sample(provider.ctx,10,&sample)==ESP_OK);
    assert(sample.valid==3 && sample.timestamp_us==(uint64_t)now && sample.acceleration_m_s2[2]>9.8f);
    unregister_result=ESP_ERR_INVALID_STATE;
    assert(solar_os_mpu6050_detach("imu0")==ESP_ERR_INVALID_STATE && regs[0x6b]==1);
    unregister_result=ESP_OK;fail_write=0x6b;
    assert(solar_os_mpu6050_detach("imu0")==ESP_FAIL); /* Keep chip state for cleanup retry. */
    fail_write=-1;assert(solar_os_mpu6050_detach("imu0")==ESP_OK);
    b[1].value=0x69;assert(solar_os_mpu6050_attach("imu0",b,3)==ESP_OK && gpio_configs==1);
    before=reads;assert(provider.ops->read_sample(provider.ctx,2,&sample)==ESP_ERR_TIMEOUT && reads==before);
    irq_level=1;regs[0x3a]=1;
    assert(provider.ops->read_sample(provider.ctx,10,&sample)==ESP_OK && reads==before+2);
    assert(solar_os_mpu6050_detach("imu0")==ESP_OK && regs[0x38]==0 && regs[0x6b]==0x40);
    assert(solar_os_mpu6050_detach("imu0")==ESP_ERR_NOT_FOUND);
    return 0;
}
