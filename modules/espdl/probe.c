#include <stdarg.h>
#include <string.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_inference_backend_abi.h"
#include "solar_os_native_inference_abi.h"
#include "fixtures.h"

static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_memory_api_v1_t *memory;
static const solar_os_native_library_service_api_v1_t *libraries;
static const solar_os_native_libc_api_v1_t *libc_api;
static solar_os_inference_model_info_t info;
static solar_os_inference_result_t output;
static int8_t a8[32],b8[32];
static float a32[8],b32[8];
static const int av[]={1,-2,3,4,-5,6,7,-8},bv[]={2,3,-4,1,6,-2,0,4};
int strcmp(const char *a,const char *b)
{ while(*a && *a==*b) {++a;++b;}return(unsigned char)*a-(unsigned char)*b; }

static void print(const char *format,...)
{
    char buffer[256];va_list args;va_start(args,format);
    int n=libc_api->format(buffer,sizeof(buffer),format,args);va_end(args);
    if(n>0)host->write_utf8(buffer,(size_t)n<sizeof(buffer)?(size_t)n:sizeof(buffer)-1);
}
static void clear_info(void)
{
    for(size_t i=0;i<SOLAR_OS_INFERENCE_PORTS_MAX;++i) {
        memory->free(info.inputs[i].exponents);memory->free(info.outputs[i].exponents);
    }
    memset(&info,0,sizeof(info));
}
static void clear_output(void)
{
    for(size_t i=0;i<SOLAR_OS_INFERENCE_PORTS_MAX;++i) {
        memory->free(output.outputs[i].tensor.exponents);memory->free(output.outputs[i].data);
    }
    memset(&output,0,sizeof(output));
}
static bool cancelled(void *user) { (void)user;return true; }
static int check(unsigned type)
{
    if(output.count!=2)return 1;
    for(size_t i=0;i<output.count;++i) {
        const solar_os_inference_tensor_t *t=&output.outputs[i].tensor;
        int sub=!strcmp(t->name,"difference");
        if(!sub && strcmp(t->name,"sum"))return 2;
        size_t n=type==2?32:8;
        if(t->bytes!=n*(type==1?4:1))return 3;
        for(size_t j=0;j<n;++j) {
            int expected=sub?av[j%8]-bv[j%8]:av[j%8]+bv[j%8];
            if(type==1) {float value;memcpy(&value,(const char*)output.outputs[i].data+j*4,4);if(value!=expected)return 4;}
            else if(((int8_t*)output.outputs[i].data)[j]!=expected)return 5;
        }
    }
    return 0;
}
#define REQUIRE(condition) do {if(!(condition)) {print("ESPDL_PROBE_FAILED line=%u\n",__LINE__);goto failed;}}while(0)

static int baseline(const char *folder)
{
    const solar_os_native_inference_api_v1_t *api=host->get_service("inference",1,sizeof(*api));
    if(!api) {print("ESPDL_BASELINE_UNAVAILABLE\n");return 1;}
    solar_os_inference_t *client=NULL;
    uint32_t model=0;
    solar_os_inference_result_t *r=NULL;
    if(api->create(NULL,NULL,&client))return 1;
    const char *names[]={"arithmetic_int8","arithmetic_float32","arithmetic_simd_int8"};
    for(unsigned type=0;type<3;++type) {
        char path[256];size_t n=strlen(folder),tail=strlen(names[type]);
        if(n+tail+8>=sizeof(path))goto failed;
        memcpy(path,folder,n);path[n++]='/';memcpy(path+n,names[type],tail);n+=tail;
        memcpy(path+n,".espdl",7);
        if(api->load(client,path,5000,&model))goto failed;
        const size_t bytes=type==1?32:type==2?32:8;
        solar_os_inference_input_t inputs[]={
            {"a",type==1?(const void*)a32:a8,bytes,NULL},
            {"b",type==1?(const void*)b32:b8,bytes,NULL}};
        uint64_t inference=0,elapsed=0;
        for(unsigned i=0;i<75;++i) {
            if(api->run(client,model,inputs,2,5000,&r))goto failed;
            output=*r;
            int error=check(type);memset(&output,0,sizeof(output));
            inference+=r->inference_us;elapsed+=r->elapsed_us;
            api->result_free(r);r=NULL;
            if(error)goto failed;
        }
        if(api->unload(client,model))goto failed;model=0;
        print("ESPDL_BASELINE %s inference_us=%llu elapsed_us=%llu\n",names[type],inference/75,elapsed/75);
    }
    api->destroy(client);print("ESPDL_BASELINE_OK runs=225\n");return 0;
failed:
    if(r)api->result_free(r);
    if(model)api->unload(client,model);
    api->destroy(client);print("ESPDL_BASELINE_FAILED\n");return 1;
}

int app_main(int argc,char **argv)
{
    (void)argc;(void)argv;
    host=solar_os_native_host_v1();
    memory=host->get_service("memory",1,sizeof(*memory));
    libraries=host->get_service("library",1,sizeof(*libraries));
    libc_api=host->get_service("libc",1,sizeof(*libc_api));
    if(!memory || !libraries || !libc_api)return 1;
    print("ESPDL_PROBE_BEGIN\n"); /* Warm the calling task's libc formatting state. */
    size_t internal_before=memory->free_bytes(false),external_before=memory->free_bytes(true);
    for(size_t i=0;i<32;++i) {a8[i]=av[i%8];b8[i]=bv[i%8];}
    for(size_t i=0;i<8;++i) {a32[i]=av[i];b32[i]=bv[i];}
    if(argc==3 && !strcmp(argv[1],"baseline"))return baseline(argv[2]);
    const uint8_t *blobs[]={fixture_int8,fixture_float32,fixture_simd,fixture_oversized};
    const size_t sizes[]={sizeof(fixture_int8),sizeof(fixture_float32),sizeof(fixture_simd),sizeof(fixture_oversized)};
    const char *names[]={"int8","float32","simd"};
    const solar_os_inference_backend_api_v1_t *api=NULL;
    uint32_t lease=0;
    void *backend=NULL;
    unsigned cycles=0;
    for(unsigned round=0;round<5;++round) for(unsigned type=0;type<3;++type) {
        const void *table=NULL;
        REQUIRE(libraries->acquire("espdl",1,sizeof(*api),&lease,&table)==0);
        api=table;
        REQUIRE(!strcmp(api->version,"3.3.13"));
        REQUIRE(libraries->unload("espdl")==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
        uint8_t invalid[16]={0};void *rejected=NULL;
        REQUIRE(api->load_buffer(invalid,sizeof(invalid),NULL,NULL,&rejected,&info)<0 && !rejected);
        clear_info();
        REQUIRE(api->load_buffer(blobs[type],sizes[type],cancelled,NULL,&rejected,&info)==SOLAR_OS_NATIVE_ERROR_TIMEOUT && !rejected);
        clear_info();
        if(round==0 && type==0) {
            REQUIRE(api->load_buffer(blobs[3],sizes[3],NULL,NULL,&rejected,&info)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !rejected);
            clear_info();print("ESPDL_OVERSIZED_REJECTED\n");
        }
        REQUIRE(api->load_buffer(blobs[type],sizes[type],NULL,NULL,&backend,&info)==0 && backend);
        if(round==0 && type==0) print("ESPDL_RESIDENT internal=%u external=%u model_internal=%u model_external=%u\n",
            (unsigned)(internal_before-memory->free_bytes(false)),
            (unsigned)(external_before-memory->free_bytes(true)),
            (unsigned)info.internal_bytes,(unsigned)info.external_bytes);
        REQUIRE(info.input_count==2 && info.output_count==2);
        REQUIRE(info.inputs[0].rank==2 && info.inputs[0].shape[0]==2 && info.inputs[0].shape[1]==(type==2?16:4));
        REQUIRE(api->set_mode(backend,SOLAR_OS_INFERENCE_DUAL)==0);
        REQUIRE(api->set_mode(backend,SOLAR_OS_INFERENCE_SINGLE)==0);
        const size_t bytes=type==1?32:type==2?32:8;
        solar_os_inference_input_t inputs[]={
            {"a",type==1?(const void*)a32:a8,bytes,NULL},
            {"b",type==1?(const void*)b32:b8,bytes,NULL}};
        REQUIRE(api->run(backend,inputs,1,NULL,NULL,&output)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
        clear_output();inputs[1].bytes--;
        REQUIRE(api->run(backend,inputs,2,NULL,NULL,&output)==SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
        clear_output();inputs[1].bytes++;
        REQUIRE(api->run(backend,inputs,2,cancelled,NULL,&output)==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
        clear_output();
        uint64_t inference=0,elapsed=0;
        for(unsigned i=0;i<75;++i) {
            REQUIRE(api->set_mode(backend,(solar_os_inference_mode_t)(i%3))==0);
            REQUIRE(api->run(backend,inputs,2,NULL,NULL,&output)==0);
            REQUIRE(check(type)==0);
            inference+=output.inference_us;elapsed+=output.elapsed_us;
            if(i!=74)clear_output();
        }
        REQUIRE(api->reset(backend)==0);
        REQUIRE(api->close(backend)==0);backend=NULL;
        clear_info();
        REQUIRE(libraries->release(lease)==0);lease=0;
        REQUIRE(libraries->unload("espdl")==0);api=NULL;
        /* DTO buffers are host-owned and remain readable after code unload. */
        REQUIRE(check(type)==0);clear_output();++cycles;
        print("ESPDL_CYCLE_OK %s inference_us=%llu elapsed_us=%llu\n",names[type],inference/75,elapsed/75);
    }
    print("ESPDL_MEMORY internal=%u:%u external=%u:%u\n",(unsigned)internal_before,
        (unsigned)memory->free_bytes(false),(unsigned)external_before,(unsigned)memory->free_bytes(true));
    print("ESPDL_PROBE_OK cycles=%u runs=%u\n",cycles,cycles*75);
    return 0;
failed:
    if(backend && api)api->close(backend);
    clear_info();clear_output();
    if(lease)libraries->release(lease);
    libraries->unload("espdl");
    return 1;
}
