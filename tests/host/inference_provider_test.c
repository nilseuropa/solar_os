#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_inference_backend.h"
#include "solar_os_inference_backend_abi.h"
#include "solar_os_native.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "freertos/task.h"
static int allocations, leases, acquired, closed, load_calls, load_error, acquire_error;
static int fail_alloc = -1, polls, cancel_at;
static size_t file_size = 16;
static bool fail_with_engine;
static const char *model_path;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls;(void)tag;if(fail_alloc==0)return NULL;if(fail_alloc>0)--fail_alloc;void *p=malloc(size);if(p)++allocations;return p; }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t cls,const char *tag)
{ void *p=solar_os_memory_alloc(n*size,cls,tag);if(p)memset(p,0,n*size);return p; }
void solar_os_memory_free(void *p) { if(p)--allocations;free(p); }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
esp_err_t solar_os_storage_stat(const char *path,solar_os_storage_metadata_t *out)
{ if(strcmp(path,model_path))return ESP_ERR_NOT_FOUND;*out=(solar_os_storage_metadata_t){.type=SOLAR_OS_STORAGE_ENTRY_FILE,.size_bytes=file_size};return ESP_OK; }
static bool cancel(void *user) { (void)user;return cancel_at && ++polls>=cancel_at; }
static int load(const uint8_t *data,size_t bytes,solar_os_inference_cancel_fn fn,void *user,
                void **out,solar_os_inference_model_info_t *info)
{
    assert(data && bytes==16 && !fn(user) && info);++load_calls;
    if(!load_error || fail_with_engine) { *out=solar_os_memory_alloc(1,0,"test");if(!*out)return -4; }
    return load_error;
}
static int run(void *engine,const solar_os_inference_input_t *inputs,size_t count,
               solar_os_inference_cancel_fn fn,void *user,solar_os_inference_result_t *out)
{ (void)inputs;(void)count;(void)fn;(void)user;(void)out;assert(engine);return -7; }
static int reset(void *engine) { assert(engine);return 0; }
static int mode(void *engine,solar_os_inference_mode_t value) { assert(engine);(void)value;return 0; }
static int close_engine(void *engine)
{
#if !SOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL
    assert(leases==1);
#endif
    assert(engine);++closed;solar_os_memory_free(engine);return 0;
}
static int load_owned(void *allocation,uint8_t *data,size_t bytes,
    solar_os_inference_cancel_fn fn,void *user,void **out,solar_os_inference_model_info_t *info)
{
    assert((uintptr_t)data%16==0 && (uintptr_t)data-(uintptr_t)allocation<=15);
    int rc=load(data,bytes,fn,user,out,info);solar_os_memory_free(allocation);return rc;
}
static solar_os_inference_backend_api_v1_t table={1,sizeof(table),"3.3.13",load,run,reset,mode,close_engine,NULL};
const solar_os_inference_backend_api_v1_t *solar_os_inference_espdl_api(void) { return &table; }
int solar_os_native_library_acquire(const char *id,uint32_t abi,uint32_t size,uint32_t *lease,const void **api)
{ assert(!strcmp(id,"espdl") && abi==1 && size==SOLAR_OS_INFERENCE_BACKEND_API_V1_SIZE);++acquired;*lease=0;*api=NULL;if(acquire_error)return acquire_error;*lease=1;*api=&table;++leases;return 0; }
int solar_os_native_library_release(uint32_t lease) { assert(lease==1 && leases==1);--leases;return 0; }
int main(int argc,char **argv)
{
    assert(argc==2);model_path=argv[1];solar_os_inference_backend_t *b=NULL;
    table.struct_size=SOLAR_OS_INFERENCE_BACKEND_API_V1_SIZE;
    solar_os_inference_model_info_t info={0};solar_os_inference_result_t output={0};
    assert(!solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info) && b);
    assert(solar_os_inference_backend_run(b,NULL,0,cancel,NULL,&output)==ESP_ERR_TIMEOUT);
    solar_os_inference_backend_reset(b);solar_os_inference_backend_set_mode(b,SOLAR_OS_INFERENCE_DUAL);
    solar_os_inference_backend_close(b);b=NULL;assert(!allocations && !leases && closed==1);
    table.version="wrong";
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_NOT_SUPPORTED && !b);
    table.version="3.3.13";assert(!allocations && !leases);
    file_size=17;
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_INVALID_SIZE && !b);
    file_size=16;assert(!allocations && !leases);
    for(int i=0;i<3;++i) {
        fail_alloc=i;
        assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_NO_MEM && !b);
        fail_alloc=-1;assert(!allocations && !leases);
    }
    cancel_at=2;polls=0;
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_TIMEOUT && !b);
    cancel_at=0;assert(!allocations && !leases);
    load_error=-4;fail_with_engine=true;
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_NO_MEM && !b);
    assert(!allocations && !leases && closed==2);
    table.struct_size=sizeof(table);table.load_owned_buffer=load_owned;load_error=0;fail_with_engine=false;
    assert(!solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info) && b);
    solar_os_inference_backend_close(b);b=NULL;assert(!allocations && !leases && closed==3);
    load_error=-4;
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_NO_MEM && !b);
    assert(!allocations && !leases);
#if SOLAR_OS_PACKAGE_BACKEND_INFERENCE_ESPDL
    assert(!acquired);
#else
    acquire_error=-5;
    assert(solar_os_inference_backend_load(model_path,cancel,NULL,&b,&info)==ESP_ERR_NOT_FOUND && !b);
    assert(!allocations && !leases);
#endif
    puts("inference provider ownership passed");return 0;
}
