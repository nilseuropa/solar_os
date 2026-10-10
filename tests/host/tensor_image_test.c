#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_tensor_image.h"
#include "solar_os_memory.h"

static unsigned allocations, yields, checks;
static bool fail_alloc;
void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t kind, const char *tag)
{
    (void)tag; assert(kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED);
    if (fail_alloc) return NULL;
    void *p = malloc(n); if (p) ++allocations; return p;
}
void solar_os_memory_free(void *p) { if (p) { --allocations; free(p); } }
int64_t esp_timer_get_time(void) { static int64_t tick; return ++tick; }
void vTaskDelay(unsigned ticks) { assert(ticks == 1); ++yields; }
static bool stop(void *user) { return ++checks >= *(unsigned *)user; }

int main(void)
{
    uint8_t rgb[] = {255,0,0, 0,255,0, 0,0,255, 10,32,13, 124,116,104, 255,255,255};
    solar_os_raster_image_pixels_t s = {rgb,sizeof(rgb),9,3,2};
    int32_t exponent = 0;
    solar_os_inference_tensor_t p = {.dtype = SOLAR_OS_TENSOR_INT8,
        .rank = 4,.shape = {1,2,3,3},.bytes = 18,.exponent_count = 1,.exponents = &exponent};
    solar_os_tensor_image_options_t o; solar_os_tensor_image_defaults(&o);
    solar_os_tensor_image_transform_t t;
    uint8_t out[3000]; memset(out,0xaa,sizeof(out));
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,18,&t,NULL,NULL) == ESP_OK);
    for (unsigned i = 0; i < sizeof(rgb); ++i) assert(out[i] == (rgb[i] > 127 ? 127 : rgb[i]));
    assert(out[18] == 0xaa && !allocations && t.preprocess_us > 0);
    assert(t.input_width == 3 && t.crop_height == 2 && t.bytes == 18);
    /* Non-integer nearest-neighbor ratios must preserve every sample coordinate. */
    p.shape[1]=5; p.shape[2]=7; p.bytes=105;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,105,&t,NULL,NULL)==ESP_OK);
    for (unsigned y=0;y<5;++y) for(unsigned x=0;x<7;++x) for(unsigned c=0;c<3;++c) {
        unsigned value=rgb[((y*2/5)*3+x*3/7)*3+c];
        assert(out[(y*7+x)*3+c]==(value>127?127:value));
    }
    p.shape[1]=2; p.shape[2]=3; p.bytes=18;
    /* Native quantization must match the previous Python classifier exactly. */
    const double mean[] = {123.675,116.28,103.53}, std[] = {58.395,57.12,57.375};
    memcpy(o.mean,mean,sizeof(mean)); memcpy(o.std,std,sizeof(std)); exponent = -6;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,18,&t,NULL,NULL) == ESP_OK);
    for (unsigned i = 0; i < sizeof(rgb); ++i) {
        int q = floor((rgb[i] - mean[i%3]) / std[i%3] * 64 + .5);
        if (q < -128) q = -128;
        if (q > 127) q = 127;
        assert(out[i] == (uint8_t)q);
    }
    /* Explicit planar layout, BGR, and per-output-channel exponents. */
    solar_os_tensor_image_defaults(&o); o.layout = SOLAR_OS_TENSOR_NCHW; o.color = SOLAR_OS_TENSOR_BGR;
    p.dtype = SOLAR_OS_TENSOR_INT16; p.shape[1] = 3; p.shape[2] = 2; p.shape[3] = 3; p.bytes = 36;
    int32_t exps[] = {0,1,2}; p.exponents = exps; p.exponent_count = 3;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,36,&t,NULL,NULL) == ESP_OK);
    for (unsigned c=0;c<3;++c) for (unsigned i=0;i<6;++i) {
        int expected = floor(rgb[i*3+2-c] / pow(2,c) + .5);
        assert((out[(c*6+i)*2] | out[(c*6+i)*2+1]<<8) == expected);
    }
    /* Floating tensors have no quantization metadata. */
    p.dtype = SOLAR_OS_TENSOR_FLOAT32; p.bytes = 72; p.exponent_count = 0;
    for (unsigned c=0;c<3;++c) { o.mean[c]=128; o.std[c]=128; }
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,72,&t,NULL,NULL) == ESP_OK);
    for (unsigned c=0;c<3;++c) for (unsigned i=0;i<6;++i) {
        float value; memcpy(&value,out+(c*6+i)*4,4);
        assert(value == (rgb[i*3+2-c]-128)/128.0f);
    }
    /* Non-batch HWC, letterbox odd padding, then crop and inverse geometry. */
    solar_os_tensor_image_defaults(&o); o.layout = SOLAR_OS_TENSOR_HWC; o.resize = SOLAR_OS_TENSOR_LETTERBOX;
    o.height=1; o.pad[0]=4; o.pad[1]=5; o.pad[2]=6;
    p = (solar_os_inference_tensor_t){.dtype=SOLAR_OS_TENSOR_UINT8,.rank=3,
        .shape={4,4,3},.bytes=48,.exponent_count=1,.exponents=&exponent}; exponent=0;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,48,&t,NULL,NULL) == ESP_OK);
    assert(t.resized_width==4 && t.resized_height==1 && t.pad_top==1);
    for (unsigned y=0;y<4;++y) for (unsigned x=0;x<4;++x) {
        const uint8_t *expected = y==1 ? rgb+(x*3/4)*3 : o.pad;
        assert(!memcmp(out+(y*4+x)*3,expected,3));
    }
    o.x=1; o.width=1; o.height=2;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,48,&t,NULL,NULL) == ESP_OK);
    assert(t.crop_x==1 && t.crop_width==1 && t.resized_width==2 && t.pad_left==1 && t.resized_height==4);
    /* Grayscale rounds with the same integer weights as native image conversion. */
    solar_os_tensor_image_defaults(&o); o.color=SOLAR_OS_TENSOR_GRAY; o.layout=SOLAR_OS_TENSOR_CHW;
    p.shape[0]=1; p.shape[1]=2; p.shape[2]=3; p.bytes=6;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,6,&t,NULL,NULL) == ESP_OK);
    for(unsigned i=0;i<6;++i) assert(out[i]==(77U*rgb[i*3]+150U*rgb[i*3+1]+29U*rgb[i*3+2])>>8);
    assert(solar_os_tensor_image_prepare(&s,&p,&o,rgb,6,&t,NULL,NULL)==ESP_ERR_INVALID_ARG);
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,5,&t,NULL,NULL)==ESP_ERR_INVALID_SIZE);
    p.dtype=SOLAR_OS_TENSOR_FLOAT16;
    assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_NOT_SUPPORTED); p.dtype=SOLAR_OS_TENSOR_UINT8;
    p.shape[0]=2; assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_INVALID_SIZE); p.shape[0]=1;
    o.x=UINT32_MAX; assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_INVALID_SIZE); o.x=0;
    o.std[0]=0; assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_INVALID_ARG); o.std[0]=1;
    o.mean[0]=NAN; assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_INVALID_ARG); o.mean[0]=0;
    s.stride=SIZE_MAX; assert(solar_os_tensor_image_inspect(&s,&p,&o,&t)==ESP_ERR_INVALID_SIZE); s.stride=9;
    fail_alloc=true; assert(solar_os_tensor_image_prepare(&s,&p,&o,out,6,&t,NULL,NULL)==ESP_ERR_NO_MEM);
    fail_alloc=false; unsigned deadline=3; checks=0;
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,6,&t,stop,&deadline)==ESP_ERR_TIMEOUT && !allocations);
    assert(solar_os_tensor_image_prepare(&s,&p,&o,out,6,&t,NULL,NULL)==ESP_OK && !allocations);
    assert(solar_os_tensor_image_option(&o,"layout","NHWC")==ESP_OK);
    assert(solar_os_tensor_image_option(&o,"color","BGR")==ESP_OK);
    assert(solar_os_tensor_image_option(&o,"resize","letterbox")==ESP_OK);
    assert(solar_os_tensor_image_option(&o,"resize","bilinear")==ESP_ERR_INVALID_ARG);
    puts("image tensor layouts/color/crop/letterbox/quantization/float/validation/OOM/cancellation passed");
    return 0;
}
