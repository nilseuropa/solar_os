#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "solar_os.h"
#include "solar_os_media_sample_source.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_media.h"
#include "solar_os_script_media.h"
#include "solar_os_pipeline.h"
#include "solar_os_memory.h"
#include "freertos/FreeRTOS.h"

static int allocations,fail_after=-1,active,leased,stop_failures,release_failures;
static bool cancel_now,timeout_source,fail_open,no_result,cancel_after_read;
static int64_t clock_us;
static const uint8_t jpeg[]={255,216,255,0,1,2,3,4};
void *solar_os_memory_alloc(size_t n,solar_os_memory_class_t kind,const char *tag)
{ (void)kind;(void)tag;if(fail_after==0)return NULL;if(fail_after>0)fail_after--;void *p=malloc(n);if(p)allocations++;return p; }
void *solar_os_memory_calloc(size_t n,size_t s,solar_os_memory_class_t kind,const char *tag)
{ void *p=solar_os_memory_alloc(n*s,kind,tag);if(p)memset(p,0,n*s);return p; }
void solar_os_memory_free(void *p) { if(p){assert(allocations>0);allocations--;free(p);} }
void *solar_os_memory_realloc(void *p, size_t size, solar_os_memory_class_t kind, const char *tag)
{ return p ? realloc(p, size) : solar_os_memory_alloc(size, kind, tag); }
bool solar_os_memory_is_external(const void *p) { return p != NULL; }
void solar_os_memory_get_status(solar_os_memory_status_t *out) { *out = (solar_os_memory_status_t){0}; }
solar_os_memory_buffer_t *solar_os_memory_buffer_create(size_t size, const char *owner)
{ (void)size; (void)owner; return NULL; }
void *solar_os_memory_buffer_pin(solar_os_memory_buffer_t *buffer) { (void)buffer; return NULL; }
bool solar_os_memory_buffer_unpin(solar_os_memory_buffer_t *buffer) { (void)buffer; return false; }
bool solar_os_memory_buffer_destroy(solar_os_memory_buffer_t *buffer) { (void)buffer; return false; }
static const solar_os_app_t owner = {.name = "media-test"};
const solar_os_app_t *solar_os_application_descriptor(void) { return &owner; }
int64_t esp_timer_get_time(void) { return clock_us+=10000; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
static bool cancel(void *user) { assert(!user);return cancel_now; }
struct solar_os_script_media { int unused; };
esp_err_t solar_os_script_media_create(const char *owner,solar_os_script_media_cancel_fn fn,void *user,solar_os_script_media_t **out)
{ assert(!strcmp(owner,"test") && fn==cancel && !user);*out=solar_os_memory_calloc(1,sizeof(**out),SOLAR_OS_MEMORY_EXTERNAL_REQUIRED,"test");return *out?ESP_OK:ESP_ERR_NO_MEM; }
void solar_os_script_media_destroy(solar_os_script_media_t *media) { assert(!active && !leased);solar_os_memory_free(media); }
esp_err_t solar_os_script_media_snapshot(solar_os_script_media_t *m,const char *source,uint16_t w,uint16_t h,uint8_t quality,uint32_t *frame)
{ (void)m;assert(!strcmp(source,"camera0") && !w && !h && !quality);active=1;if(fail_open)return ESP_FAIL;leased=1;*frame=2;return ESP_OK; }
esp_err_t solar_os_script_media_frame(solar_os_script_media_t *m,uint32_t frame,solar_os_script_media_frame_t *out)
{ (void)m;assert(frame==2 && leased);*out=(solar_os_script_media_frame_t){.jpeg={jpeg,sizeof(jpeg),640,480,1234}};return ESP_OK; }
esp_err_t solar_os_script_media_release(solar_os_script_media_t *m,uint32_t frame)
{ (void)m;assert(frame==2 && leased);if(release_failures){release_failures--;return ESP_FAIL;}leased=0;return ESP_OK; }
esp_err_t solar_os_script_media_close_all(solar_os_script_media_t *m)
{ if(leased){esp_err_t e=solar_os_script_media_release(m,2);if(e)return e;}if(stop_failures){stop_failures--;return ESP_FAIL;}active=0;return ESP_OK; }
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
esp_err_t solar_os_script_media_rtsp_open(solar_os_script_media_t *m,const char *url,bool video,bool audio,uint32_t *handle)
{ (void)m;assert(!strcmp(url,"rtsp://test/media") && video && !audio);active=1;*handle=1;return fail_open?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_script_media_rtsp_read(solar_os_script_media_t *m,uint32_t handle,uint32_t timeout,uint32_t *frame)
{ (void)m;assert(handle==1 && timeout==50);*frame=timeout_source?0:2;leased=*frame!=0;if(cancel_after_read)cancel_now=true;return ESP_OK; }
esp_err_t solar_os_script_media_rtsp_status(solar_os_script_media_t *m,uint32_t handle,solar_os_rtsp_client_status_t *out,bool *ended)
{ (void)m;(void)handle;*out=(solar_os_rtsp_client_status_t){.video=true};*ended=false;return ESP_OK; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
esp_err_t solar_os_pipeline_result_sample(uint32_t id,uint64_t after,char **json,solar_os_media_sample_t **out)
{ assert(id==1 && !after && !json);*out=NULL;return no_result?ESP_OK:solar_os_media_sample_copy_image(jpeg,sizeof(jpeg),NULL,out); }
#endif

typedef struct {
    const solar_os_native_media_sample_api_v1_t *api;
    solar_os_native_handle_t handle;
    const uint8_t *data;
    size_t size;
    const char *source, *encoding;
    uint32_t kind, width;
} expected_sample_t;

static int check_sample(const solar_os_native_media_sample_view_v1_t *view, void *user)
{
    const expected_sample_t *expected = user;
    assert(view->kind == expected->kind && view->metadata.width == expected->width);
    assert(view->length == expected->size && !memcmp(view->data, expected->data, expected->size));
    assert(!strcmp(view->encoding, expected->encoding) && !strcmp(view->metadata.source, expected->source));
    assert(expected->api->close(expected->handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    return SOLAR_OS_NATIVE_OK;
}

static void test_native_samples(const char *path, const solar_os_media_sample_source_options_t *options)
{
    const solar_os_native_media_sample_api_v1_t *api = solar_os_native_media_get_service(
        SOLAR_OS_NATIVE_MEDIA_SAMPLE_SERVICE, SOLAR_OS_NATIVE_MEDIA_SAMPLE_ABI, sizeof(*api));
    assert(api);
    solar_os_native_handle_t id = 0;
    assert(!api->open(SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE, path, sizeof(jpeg), cancel, NULL, &id));
    expected_sample_t expected = {.api = api, .handle = id, .data = jpeg, .size = sizeof(jpeg),
        .source = path, .encoding = "image/jpeg", .kind = SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE};
    assert(!api->visit(id, check_sample, &expected));
    solar_os_native_handle_t copy = 0;
    assert(!api->retain(id, &copy) && copy != id && allocations == 1);
    assert(!api->close(id));
    assert(api->visit(id, check_sample, &expected) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    expected.handle = copy;
    assert(!api->visit(copy, check_sample, &expected));
    assert(!api->close(copy) && !allocations);
    assert(api->open(SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE, path, 1, NULL, NULL, &id) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    cancel_now = true;
    assert(api->open(SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE, path, sizeof(jpeg), cancel, NULL, &id) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    cancel_now = false;

    char source[] = "copied";
    uint8_t wav[] = "RIFF\0\0\0\0WAVE";
    const uint8_t expected_wav[] = "RIFF\0\0\0\0WAVE";
    const solar_os_media_sample_metadata_t metadata = {.source = source};
    assert(!api->copy(SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO, wav, sizeof(wav), &metadata, &id));
    wav[0] = 0;
    source[0] = 0;
    expected = (expected_sample_t){.api = api, .handle = id, .data = expected_wav, .size = sizeof(expected_wav),
        .source = "copied", .encoding = "wav", .kind = SOLAR_OS_NATIVE_MEDIA_SAMPLE_AUDIO};
    assert(!api->visit(id, check_sample, &expected));
    assert(!solar_os_native_handle_close_owner(&owner) && !allocations);
    assert(api->close(id) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);

    stop_failures = 1;
    assert(!api->read_source("stream:camera0", options, &id) && !leased && !active);
    expected = (expected_sample_t){.api = api, .handle = id, .data = jpeg, .size = sizeof(jpeg),
        .source = "stream:camera0", .encoding = "image/jpeg", .kind = SOLAR_OS_NATIVE_MEDIA_SAMPLE_IMAGE, .width = 640};
    assert(!api->visit(id, check_sample, &expected));
    assert(!api->close(id));
}

int main(void)
{
    (void)timeout_source; (void)no_result; (void)cancel_after_read;
    solar_os_media_sample_source_options_t options={.owner="test",.max_bytes=512*1024,.timeout_ms=100,.cancel=cancel};
    solar_os_media_sample_t *sample=NULL;
    char path[128];snprintf(path,sizeof(path),"/tmp/solaros-source-%ld.jpg",(long)getpid());
    FILE *file=fopen(path,"wb");assert(file && fwrite(jpeg,1,sizeof(jpeg),file)==sizeof(jpeg));fclose(file);
    test_native_samples(path, &options);
    assert(!solar_os_media_sample_source_read(path,&options,&sample));
    assert(!memcmp(solar_os_media_sample_image(sample)->data,jpeg,sizeof(jpeg)));solar_os_media_sample_release(sample);
    options.max_bytes=1;assert(solar_os_media_sample_source_read(path,&options,&sample)==ESP_ERR_INVALID_SIZE && !sample && !allocations);
    options.max_bytes=512*1024;assert(!unlink(path));
    stop_failures=2;assert(!solar_os_media_sample_source_read("stream:camera0",&options,&sample));
    assert(!active && !leased && solar_os_media_sample_image(sample)->metadata.timestamp_us==1234);
    solar_os_media_sample_release(sample);assert(!allocations);
    release_failures=2;assert(solar_os_media_sample_source_read("stream:camera0",&options,&sample)==ESP_FAIL);
    assert(!sample && !leased && !active && !allocations);
    options.max_bytes=1;assert(solar_os_media_sample_source_read("stream:camera0",&options,&sample)==ESP_ERR_INVALID_SIZE);
    assert(!sample && !leased && !active && !allocations);options.max_bytes=512*1024;
    fail_open=true;assert(solar_os_media_sample_source_read("stream:camera0",&options,&sample)==ESP_FAIL);
    assert(!sample && !leased && !active && !allocations);fail_open=false;
    cancel_now=true;assert(solar_os_media_sample_source_read("stream:camera0",&options,&sample)==ESP_ERR_INVALID_STATE);cancel_now=false;
    for(int i=0;i<3;i++){fail_after=i;esp_err_t e=solar_os_media_sample_source_read("stream:camera0",&options,&sample);fail_after=-1;assert(e==ESP_OK || e==ESP_ERR_NO_MEM);solar_os_media_sample_release(sample);assert(!allocations && !active && !leased);}
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    assert(!solar_os_media_sample_source_read("rtsp://test/media",&options,&sample));solar_os_media_sample_release(sample);
    timeout_source=true;assert(solar_os_media_sample_source_read("rtsp://test/media",&options,&sample)==ESP_ERR_TIMEOUT && !sample);timeout_source=false;
    cancel_after_read=true;assert(solar_os_media_sample_source_read("rtsp://test/media",&options,&sample)==ESP_ERR_INVALID_STATE && !sample);cancel_after_read=cancel_now=false;
#else
    assert(solar_os_media_sample_source_read("rtsp://test/media",&options,&sample)==ESP_ERR_NOT_SUPPORTED);
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
    assert(!solar_os_media_sample_source_read("pipeline:1",&options,&sample));solar_os_media_sample_release(sample);
    assert(solar_os_media_sample_source_read("pipeline:-1",&options,&sample)==ESP_ERR_INVALID_ARG);
    no_result=true;assert(solar_os_media_sample_source_read("pipeline:1",&options,&sample)==ESP_ERR_NOT_FOUND);
#else
    assert(solar_os_media_sample_source_read("pipeline:1",&options,&sample)==ESP_ERR_NOT_SUPPORTED);
#endif
    assert(!allocations && !active && !leased);
    puts("Encoded source ownership, bounds, file/stream/RTSP/pipeline, cancellation and OOM passed");
}
