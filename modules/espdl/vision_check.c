#include <stdarg.h>
#include <string.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_inference_abi.h"
static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_libc_api_v1_t *libc_api;
static const solar_os_native_library_service_api_v1_t *libraries;
static const solar_os_native_inference_api_v1_t *api;
static solar_os_inference_input_t inputs[SOLAR_OS_INFERENCE_PORTS_MAX];
static void print(const char *format,...)
{
    char text[256];va_list args;va_start(args,format);
    int n=libc_api->format(text,sizeof(text),format,args);va_end(args);
    if(n>0)host->write_utf8(text,(size_t)n<sizeof(text)?(size_t)n:sizeof(text)-1);
}
static uint32_t hash(const solar_os_inference_result_t *r)
{
    uint32_t h=2166136261U;
    for(size_t i=0;i<r->count;++i) {
        const unsigned char *p=r->outputs[i].data;
        for(size_t j=0;j<r->outputs[i].tensor.bytes;++j) h=(h^p[j])*16777619U;
    }
    return h;
}
static void clear_inputs(void)
{
    for(size_t i=0;i<SOLAR_OS_INFERENCE_PORTS_MAX;++i)memory->free((void*)inputs[i].data);
    memset(inputs,0,sizeof(inputs));
}
static unsigned cancel_polls;
static bool cancel(void *user)
{ (void)user;return cancel_polls && --cancel_polls==0; }
#define REQUIRE(value) do {int rc=(value);if(rc) {print("VISION_FAILED line=%u rc=%d\n",__LINE__,rc);goto failed;}}while(0)
int app_main(int argc,char **argv)
{
    host=solar_os_native_host_v1();
    memory=host->get_service("memory",1,sizeof(*memory));
    libc_api=host->get_service("libc",1,sizeof(*libc_api));
    libraries=host->get_service("library",1,sizeof(*libraries));
    api=host->get_service("inference",1,sizeof(*api));
    if(argc!=2 || !memory || !libc_api || !api || !libraries)return 1;
    print("VISION_BEGIN %s\n",argv[1]);
    size_t internal=0,external=0;
    solar_os_inference_t *client=NULL;uint32_t model=0;
    solar_os_inference_result_t *output=NULL;
    const solar_os_inference_model_info_t *info=NULL;
    uint32_t expected[2]={0};
    REQUIRE(api->create(cancel,NULL,&client));
    /* Built-in ModuleCreator caches its registry for the firmware lifetime.
     * Warm that one-time state before checking growth across model cycles. */
    REQUIRE(api->load(client,argv[1],60000,&model));
    REQUIRE(api->unload(client,model));model=0;
    int warm_unload=libraries->unload("espdl");
    if(warm_unload && warm_unload!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)goto failed;
    internal=memory->free_bytes(false);external=memory->free_bytes(true);
    for(unsigned round=0;round<3;++round) {
        REQUIRE(api->load(client,argv[1],60000,&model));
        REQUIRE(api->info(client,model,&info));
        print("VISION_MODEL round=%u bytes=%u workspace_internal=%u workspace_external=%u resident_internal=%u resident_external=%u\n",
            round,(unsigned)info->model_bytes,(unsigned)info->internal_bytes,(unsigned)info->external_bytes,
            (unsigned)(internal-memory->free_bytes(false)),(unsigned)(external-memory->free_bytes(true)));
        if(info->input_count>SOLAR_OS_INFERENCE_PORTS_MAX)goto failed;
        for(size_t i=0;i<info->input_count;++i) {
            inputs[i]=(solar_os_inference_input_t){info->inputs[i].name,
                memory->alloc(info->inputs[i].bytes),info->inputs[i].bytes,&info->inputs[i]};
            if(!inputs[i].data)goto failed;
        }
        /* The frontend model lease must block native library unload. */
        int unload=libraries->unload("espdl");
        if(unload && unload!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND && unload!=SOLAR_OS_NATIVE_ERROR_INVALID_STATE)goto failed;
        for(unsigned mode=0;mode<3;++mode) {
            REQUIRE(api->set_mode(client,model,(solar_os_inference_mode_t)mode));
            for(unsigned seed=0;seed<2;++seed) {
                for(size_t i=0;i<info->input_count;++i) {
                    unsigned char *p=(unsigned char*)inputs[i].data;
                    for(size_t j=0;j<inputs[i].bytes;++j)p[j]=seed?(j*17+31)%127:0;
                }
                REQUIRE(api->run(client,model,inputs,info->input_count,60000,&output));
                uint32_t value=hash(output);
                if(!round && !mode)expected[seed]=value;
                if(value!=expected[seed]) {print("VISION_MISMATCH expected=%08x actual=%08x\n",expected[seed],value);goto failed;}
                print("VISION_RUN round=%u mode=%u seed=%u hash=%08x inference_us=%llu elapsed_us=%llu resident_internal=%u resident_external=%u\n",
                    round,mode,seed,value,output->inference_us,output->elapsed_us,
                    (unsigned)(internal-memory->free_bytes(false)),(unsigned)(external-memory->free_bytes(true)));
                api->result_free(output);output=NULL;
            }
        }
        cancel_polls=3;
        if(api->run(client,model,inputs,info->input_count,60000,&output)!=SOLAR_OS_NATIVE_ERROR_TIMEOUT)goto failed;
        api->result_free(output);output=NULL;cancel_polls=0;
        REQUIRE(api->run(client,model,inputs,info->input_count,60000,&output));
        uint32_t final_hash=hash(output);
        REQUIRE(api->unload(client,model));model=0;
        unload=libraries->unload("espdl");
        if(unload && unload!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)goto failed;
        if(hash(output)!=final_hash)goto failed;
        api->result_free(output);output=NULL;clear_inputs();
    }
    api->destroy(client);
    print("VISION_MEMORY internal=%u:%u external=%u:%u\n",(unsigned)internal,
        (unsigned)memory->free_bytes(false),(unsigned)external,(unsigned)memory->free_bytes(true));
    print("VISION_OK rounds=3 runs=21 hashes=%08x:%08x\n",expected[0],expected[1]);return 0;
failed:
    cancel_polls=0;
    if(output)api->result_free(output);
    clear_inputs();if(model)api->unload(client,model);
    if(client)api->destroy(client);
    libraries->unload("espdl");return 1;
}
