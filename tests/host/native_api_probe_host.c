#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"

static bool display;
static unsigned writes, sleeps, allocations, images;
static int64_t now_us(void) { return 123456; }
static void sleep_ms(uint32_t ms) { assert(ms == 0); ++sleeps; }
static void *allocate(size_t bytes) { assert(bytes == 64); ++allocations; return malloc(bytes); }
static void release(void *buffer) { assert(buffer && allocations); --allocations; free(buffer); }
static const solar_os_native_time_api_v1_t clock_api = {
    .abi_version=1, .struct_size=sizeof(clock_api), .now_us=now_us, .sleep_ms=sleep_ms,
};
/* Simulate an older provider exposing only its original alloc/free prefix. */
static const solar_os_native_memory_api_v1_t memory_api = {
    .abi_version=1,
    .struct_size=SOLAR_OS_NATIVE_THROUGH(solar_os_native_memory_api_v1_t, free),
    .alloc=allocate, .free=release,
};
static const solar_os_native_image_api_v1_t image_api = { .abi_version=1, .struct_size=8 };
static const void *lookup(const char *, uint32_t, uint32_t);
static uint32_t service_count(void) { return 4; }
static int service_info(uint32_t index, solar_os_native_system_service_v1_t *info)
{
    static const char *names[] = {"time", "memory", "system", "image"};
    assert(index < 4 && info);
    memset(info,0,sizeof(*info));
    strcpy(info->name,names[index]); info->abi_version=1;
    const void *value=lookup(info->name,1,8);
    if(value) {
        uint32_t header[2];memcpy(header,value,sizeof(header));
        info->struct_size=header[1];info->flags=SOLAR_OS_NATIVE_SYSTEM_AVAILABLE;
    }
    return 0;
}
static const solar_os_native_system_api_v1_t system_api = {
    .abi_version=1, .struct_size=sizeof(system_api),
    .service_count=service_count, .service_info=service_info,
};
static const void *lookup(const char *name, uint32_t abi, uint32_t size)
{
    if (!name || abi!=1) return NULL;
    const void *value=NULL;
    if(!strcmp(name,"time")) value=&clock_api;
    if(!strcmp(name,"memory")) value=&memory_api;
    if(!strcmp(name,"system")) value=&system_api;
    if(!strcmp(name,"image") && display) { value=&image_api; ++images; }
    if(!value) return NULL;
    uint32_t header[2];memcpy(header,value,sizeof(header));
    return size<=header[1] ? value : NULL;
}
static int write_utf8(const char *text,size_t bytes)
{
    assert(bytes==20 && !memcmp(text,"NATIVE_API_PROBE_OK\n",20));
    ++writes;return 0;
}
const solar_os_native_host_api_v1_t *solar_os_native_host_v1(void)
{
    static const solar_os_native_host_api_v1_t host = {
        .abi_version=1,.struct_size=sizeof(host),.target="test",
        .firmware_version="test",.write_utf8=write_utf8,.get_service=lookup,
    };
    return &host;
}
int main(int argc,char **argv)
{
    assert(argc==3);display=!strcmp(argv[2],"display");
    void *module=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
    if(!module) { fputs(dlerror(),stderr); return 1; }
    int (*entry)(int,char **)=NULL;
    void *symbol=dlsym(module,"main");
    memcpy(&entry,&symbol,sizeof(entry));assert(entry);
    assert(!entry(0,NULL));
    assert(writes==1 && sleeps==1 && !allocations);
    assert((images!=0)==display);
    assert(!dlclose(module));
    printf("identical native probe passed: %s\n",argv[2]);return 0;
}
