#include <assert.h>
#include <math.h>
#include <string.h>
#include "mpu6050.h"

typedef struct {
    uint8_t regs[256]; unsigned writes, delay_ms;
    int fail_write, fail_read; bool stuck;
} fake_t;
static fake_t fresh(void)
{ fake_t b={.fail_write=-1,.fail_read=-1};b.regs[0x75]=0x68;return b; }
static esp_err_t read_regs(void *ctx,uint8_t reg,uint8_t *data,size_t size)
{
    fake_t *b=ctx;
    if(reg==b->fail_read) return ESP_FAIL;
    if(reg==0x6b && !b->stuck) b->regs[reg]&=~0x80;
    if(reg==0x3b) assert(size==14);
    memcpy(data,b->regs+reg,size);
    if(reg==0x3a) b->regs[reg]=0;
    return ESP_OK;
}
static esp_err_t write_regs(void *ctx,uint8_t reg,const uint8_t *data,size_t size)
{
    fake_t *b=ctx;assert(size==1);
    if((int)b->writes++==b->fail_write) return ESP_FAIL;
    b->regs[reg]=*data;return ESP_OK;
}
static void delay(void *ctx,uint32_t ms) { ((fake_t *)ctx)->delay_ms+=ms; }
static esp_err_t init(fake_t *b,mpu6050_t *d)
{ const mpu6050_io_t io={.read=read_regs,.write=write_regs,.delay_ms=delay,.ctx=b};return mpu6050_init(d,&io); }
int main(void)
{
    fake_t b=fresh();mpu6050_t d;
    assert(mpu6050_init(&d,NULL)==ESP_ERR_INVALID_ARG);
    b.regs[0x75]=0x70;assert(init(&b,&d)==ESP_ERR_NOT_FOUND && !d.initialized && !b.writes);
    b=fresh();b.fail_read=0x75;assert(init(&b,&d)==ESP_FAIL && !b.writes);
    b=fresh();b.stuck=true;assert(init(&b,&d)==ESP_ERR_TIMEOUT && !d.initialized && b.regs[0x6b]==0x40);
    /* Every initialization write can fail without leaving enabled sensors/IRQ. */
    for(int i=0;i<9;++i) {
        b=fresh();b.fail_write=i;assert(init(&b,&d)==ESP_FAIL && !d.initialized);
        assert(b.regs[0x38]==0 && b.regs[0x6b]==0x40);
    }
    b=fresh();assert(init(&b,&d)==ESP_OK && b.delay_ms==200);
    assert(b.regs[0x6b]==1 && b.regs[0x6c]==0 && b.regs[0x19]==9 && b.regs[0x1a]==3);
    assert(b.regs[0x1b]==0 && b.regs[0x1c]==0 && b.regs[0x37]==0x20 && b.regs[0x38]==1);
    bool ready=true;assert(mpu6050_data_ready(&d,&ready)==ESP_OK && !ready);
    b.regs[0x3a]=1;assert(mpu6050_data_ready(&d,&ready)==ESP_OK && ready);
    assert(mpu6050_data_ready(&d,&ready)==ESP_OK && !ready);
    const int16_t raw[]={16384,-16384,32767,999,131,-131,-32768};
    for(unsigned i=0;i<7;++i) { b.regs[0x3b+2*i]=(uint16_t)raw[i]>>8;b.regs[0x3c+2*i]=(uint16_t)raw[i]; }
    mpu6050_sample_t s;
    assert(mpu6050_read_sample(&d,&s)==ESP_OK);
    for(unsigned i=0;i<3;++i) {
        assert(fabsf(s.acceleration_m_s2[i]-raw[i]*(9.80665f/16384))<1e-5);
        assert(fabsf(s.angular_velocity_rad_s[i]-raw[i+4]*(0.01745329252f/131))<1e-5);
    }
    mpu6050_sample_t previous=s;b.fail_read=0x3b;
    assert(mpu6050_read_sample(&d,&s)==ESP_FAIL && !memcmp(&s,&previous,sizeof(s)));
    b.fail_read=-1;b.fail_write=b.writes;
    assert(mpu6050_deinit(&d)==ESP_FAIL && d.initialized);
    b.fail_write=-1;assert(mpu6050_deinit(&d)==ESP_OK && !d.initialized && b.regs[0x6b]==0x40 && b.regs[0x38]==0);
    return 0;
}
