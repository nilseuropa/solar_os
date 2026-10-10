#define main native_qr_suite
#include "vision_test.c"
#undef main
#include "solar_os_imlib.h"
#include <math.h>

static solar_os_raster_image_t *gray_image(const uint8_t *data, unsigned w, unsigned h)
{
    solar_os_raster_image_t *image = NULL;
    assert(solar_os_raster_image_from_pixels(data, (size_t)w*h, w, h,
        SOLAR_OS_RASTER_IMAGE_GRAY8, 0, &image) == ESP_OK);
    return image;
}
static solar_os_imlib_result_t *run_imlib(solar_os_raster_image_t *image,
    solar_os_raster_image_t *other, solar_os_imlib_operation_t op, solar_os_imlib_options_t *o)
{
    solar_os_imlib_result_t *result = NULL;
    assert(solar_os_imlib_run(image, other, op, o, NULL, NULL, &result) == ESP_OK);
    assert(result && !workers);
    return result;
}
static unsigned gray_at(solar_os_raster_image_t *image, unsigned x, unsigned y)
{
    solar_os_raster_image_pixels_t p;
    assert(solar_os_raster_image_pixels(image, &p) == ESP_OK);
    unsigned index = y*p.stride + x*3;
    assert(p.data[index] == p.data[index+1] && p.data[index] == p.data[index+2]);
    return p.data[index];
}
static bool abort_request(void *user) { return ++*(unsigned *)user >= 2; }
#ifndef IMLIB_TEST_ENTRY
#define IMLIB_TEST_ENTRY main
#endif
int IMLIB_TEST_ENTRY(void)
{
    uint8_t gray[32*24]; memset(gray, 20, sizeof(gray));
    for (unsigned y = 6; y < 12; ++y) for (unsigned x = 8; x < 16; ++x) gray[y*32+x] = 200;
    gray[2*32+2] = 200;
    solar_os_raster_image_t *image = gray_image(gray, 32, 24);
    int baseline = allocations;
    solar_os_imlib_options_t o = solar_os_imlib_default_options();
    o.image = (solar_os_raster_image_convert_options_t){.x=8,.y=6,.width=8,.height=6};
    solar_os_imlib_result_t *r = run_imlib(image, NULL, SOLAR_OS_IMLIB_STATISTICS, &o);
    assert(r->statistics[0].mean == 200 && r->statistics[0].min == 200 && r->statistics[0].max == 200);
    assert(r->statistics[0].stdev == 0 && fabsf(r->histogram[0][200]-1) < 0.00001);
    solar_os_imlib_result_free(r); assert(allocations == baseline);
    o = solar_os_imlib_default_options(); o.threshold_count = 1; o.thresholds[0].l_min=128; o.thresholds[0].l_max=255;
    r = run_imlib(image, NULL, SOLAR_OS_IMLIB_BLOBS, &o);
    assert(r->count == 1 && !r->truncated);
    solar_os_imlib_blob_t b = r->blobs[0];
    assert(b.x == 8 && b.y == 6 && b.width == 8 && b.height == 6 && b.pixels == 48);
    assert(fabsf(b.cx-11.5f) < 0.01 && fabsf(b.cy-8.5f) < 0.01);
    solar_os_imlib_result_free(r);
    o.image.x=4; o.image.y=2; o.image.width=20; o.image.height=16;
    o.image.output_width=10; o.image.output_height=8;
    r = run_imlib(image, NULL, SOLAR_OS_IMLIB_BLOBS, &o);
    assert(r->count == 1 && r->blobs[0].x==8 && r->blobs[0].y==6 && r->blobs[0].pixels==12);
    solar_os_imlib_result_free(r);
    o.image=(solar_os_raster_image_convert_options_t){0};
    r = run_imlib(image, NULL, SOLAR_OS_IMLIB_BINARY, &o);
    assert(gray_at(r->image, 8,6)==255 && gray_at(r->image, 0,0)==0 && gray_at(image,8,6)==200);
    solar_os_raster_image_t *mask = r->image; r->image=NULL; solar_os_imlib_result_free(r);
    r = run_imlib(mask, NULL, SOLAR_OS_IMLIB_OPENING, NULL);
    assert(gray_at(r->image,2,2)==0 && gray_at(r->image,10,8)==255);
    solar_os_imlib_result_free(r);
    r = run_imlib(mask, NULL, SOLAR_OS_IMLIB_ERODE, NULL);
    assert(gray_at(r->image,8,6)==0 && gray_at(r->image,10,8)==255);
    solar_os_imlib_result_free(r);
    r = run_imlib(mask, NULL, SOLAR_OS_IMLIB_DILATE, NULL);
    assert(gray_at(r->image,7,6)==255); solar_os_imlib_result_free(r);
    r = run_imlib(mask, NULL, SOLAR_OS_IMLIB_CLOSING, NULL);
    assert(gray_at(r->image,10,8)==255); solar_os_imlib_result_free(r);
    r = run_imlib(image, image, SOLAR_OS_IMLIB_DIFFERENCE, NULL);
    assert(gray_at(r->image,8,6)==0); solar_os_imlib_result_free(r);
    r = run_imlib(image, NULL, SOLAR_OS_IMLIB_INVERT, NULL);
    assert(gray_at(r->image,8,6)==55 && gray_at(r->image,0,0)==235); solar_os_imlib_result_free(r);
    uint8_t impulse[9*9]={0}; impulse[4*9+4]=255;
    solar_os_raster_image_t *noise=gray_image(impulse,9,9);
    r=run_imlib(noise,NULL,SOLAR_OS_IMLIB_MEDIAN,NULL); assert(gray_at(r->image,4,4)==0); solar_os_imlib_result_free(r);
    r=run_imlib(noise,NULL,SOLAR_OS_IMLIB_MEAN,NULL); assert(gray_at(r->image,4,4)>=27 && gray_at(r->image,4,4)<=29); solar_os_imlib_result_free(r);
    r=run_imlib(noise,NULL,SOLAR_OS_IMLIB_GAUSSIAN,NULL); assert(gray_at(r->image,4,4)>=63 && gray_at(r->image,4,4)<=64); solar_os_imlib_result_free(r);
    solar_os_raster_image_release(noise);
    uint8_t rgb[20*10*3]={0};
    for (unsigned y=2;y<7;++y) for(unsigned x=3;x<9;++x) rgb[(y*20+x)*3]=255;
    solar_os_raster_image_t *color=NULL;
    assert(solar_os_raster_image_from_pixels(rgb,sizeof(rgb),20,10,SOLAR_OS_RASTER_IMAGE_RGB888,0,&color)==ESP_OK);
    o=solar_os_imlib_default_options(); o.image.format=SOLAR_OS_RASTER_IMAGE_RGB565_LE;
    r=run_imlib(color,NULL,SOLAR_OS_IMLIB_HISTOGRAM,&o); assert(r->channels==3 && r->bins[0]==101); solar_os_imlib_result_free(r);
    o.threshold_count=1; o.thresholds[0]=(solar_os_imlib_threshold_t){20,80,30,127,0,127};
    r=run_imlib(color,NULL,SOLAR_OS_IMLIB_BLOBS,&o); assert(r->count==1 && r->blobs[0].pixels==30); solar_os_imlib_result_free(r);
    solar_os_raster_image_release(color);
    o=solar_os_imlib_default_options(); o.threshold_count=1; o.thresholds[0].l_min=128; o.thresholds[0].l_max=255;
    o.pixels_threshold=1; o.area_threshold=1; o.max_blobs=1;
    r=run_imlib(image,NULL,SOLAR_OS_IMLIB_BLOBS,&o); assert(r->count==1 && r->truncated); solar_os_imlib_result_free(r);
    o.max_blobs=32; o.merge=true; o.margin=8;
    r=run_imlib(image,NULL,SOLAR_OS_IMLIB_BLOBS,&o); assert(r->count==1 && r->blobs[0].pixels==49 && r->blobs[0].count==2); solar_os_imlib_result_free(r);
    solar_os_raster_image_release(mask); assert(allocations==baseline);
    // Every allocation boundary must fail without leaking or invalidating inputs.
    o.merge=false;
    for (unsigned op=0;op<2;++op) for (int i=0;i<30;++i) {
        failures=i; r=NULL;
        esp_err_t e=solar_os_imlib_run(image,NULL,op ? SOLAR_OS_IMLIB_GAUSSIAN : SOLAR_OS_IMLIB_BLOBS,&o,NULL,NULL,&r);
        failures=-1;
        assert(e==ESP_OK || e==ESP_ERR_NO_MEM); solar_os_imlib_result_free(r);
        assert(allocations==baseline && !workers && gray_at(image,8,6)==200);
    }
    deny_worker=true; r=NULL;
    assert(solar_os_imlib_run(image,NULL,SOLAR_OS_IMLIB_STATISTICS,NULL,NULL,NULL,&r)==ESP_ERR_NO_MEM);
    deny_worker=false; assert(allocations==baseline);
    unsigned calls=0;
    assert(solar_os_imlib_run(image,NULL,SOLAR_OS_IMLIB_BLOBS,&o,abort_request,&calls,&r)==ESP_ERR_TIMEOUT);
    assert(!r && !workers && allocations==baseline);
    o.image.x=32;
    assert(solar_os_imlib_run(image,NULL,SOLAR_OS_IMLIB_BLOBS,&o,NULL,NULL,&r)==ESP_ERR_INVALID_SIZE);
    solar_os_raster_image_release(image); assert(!allocations);
    puts("imlib subset, image ownership, ROI, merging, cancellation, and allocation failures: OK");
    return 0;
}
