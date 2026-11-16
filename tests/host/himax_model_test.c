/* Production provider + native data services, with deterministic SSCMA/SPI and
 * POSIX storage adapters. No test communicates with physical hardware. */
#include <stdbool.h>
static bool admit_memory(void);
static bool admit_write(const char *path);
#define SOLAR_OS_TEST_MEMORY_ADMIT admit_memory
#define SOLAR_OS_TEST_WRITE_ADMIT admit_write
#define main zoo_fixture_main
#define solar_os_storage_flash_is_mounted zoo_fixture_flash_is_mounted
#include "zoo_test.c"
#undef solar_os_storage_flash_is_mounted
#undef main
#include "solar_os_native_data.h"
#include "solar_os_native_data_abi.h"
#include "solar_os_native_sscma_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_json_scan.h"
#include "../../modules/himax/manager.h"

static unsigned owner, selected=1, pages, resets, probes, raw_calls, slept;
static bool ota, sensor;
static uint32_t target_address;
static uint8_t programmed[2*1024*1024];
static size_t programmed_size;
static uint64_t clock_us;
static int no_memory_after = -1;
bool solar_os_storage_flash_is_mounted(void) { return strcmp(mode,"no-flash") != 0 && strcmp(mode,"slots-no-flash") != 0; }
static bool admit_memory(void)
{
    if (no_memory_after == 0) return false;
    if (no_memory_after > 0) --no_memory_after;
    return true;
}
static void *provider_alloc(size_t size) { return solar_os_memory_alloc(size,0,"model.test"); }
static bool admit_write(const char *path)
{return !strstr(path,"/.himax/") || (strcmp(mode,"record-failure") && (strcmp(mode,"final-record-failure") || !pages));}
esp_err_t solar_os_crypto_bytes_to_hex(const uint8_t *data,size_t n,char *out,size_t size)
{
    if(size < 2*n+1)return ESP_ERR_INVALID_SIZE;
    for(size_t i=0;i<n;i++)snprintf(out+i*2,3,"%02x",data[i]);
    return 0;
}
static uint32_t get32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static void set32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;i++)p[i]=n>>(i*8);}
static int open_camera(const char *source,const char *who,uint32_t *token)
{
    assert(who&&*who);*token=0;
    if(strcmp(source,"camera0")&&strcmp(source,"camera1"))return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    if(owner||!strcmp(mode,"busy"))return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    owner=1;*token=77;return 0;
}
static int start_camera(uint32_t token) {assert(token==77&&owner&&!ota);sensor=true;return 0;}
static int close_camera(uint32_t token) {assert(token==77&&owner);owner=0;sensor=false;return 0;}
static int exchange_camera(uint32_t token,const char *source,const char *command,const char *name,bool event,
    uint32_t timeout,solar_os_native_cancel_fn stop,void *user,char **reply)
{
    assert(token==77&&owner&&(!strcmp(source,"camera0")||!strcmp(source,"camera1"))&&timeout&&!ota);
    *reply=NULL;if(stop&&stop(user))return SOLAR_OS_NATIVE_ERROR_TIMEOUT;
    char data[1024];
    if(!strcmp(name,"MODEL?"))snprintf(data,sizeof(data),"{\"data\":{\"id\":%u}}",selected);
    else if(!strcmp(name,"MODEL")) {
        unsigned slot;assert(sscanf(command,"AT+MODEL=%u",&slot)==1&&slot>=1&&slot<=4);
        if(!strcmp(mode,"select-failure")&&slot==4)return SOLAR_OS_NATIVE_ERROR_FAILED;
        selected=slot;snprintf(data,sizeof(data),"{\"data\":{\"id\":%u}}",selected);
    } else if(!strcmp(name,"VER?"))snprintf(data,sizeof(data),"{\"data\":{\"software\":\"%s\",\"at_api\":\"v0\"}}",!strcmp(mode,"old-firmware")?"2024.05.01":"2024.08.16");
    else if(!strcmp(name,"ID?"))snprintf(data,sizeof(data),"{\"data\":\"%s\"}",!strcmp(mode,"slots-other-device")?"fedcba9876543210":"0123456789abcdef");
    else if(!strcmp(name,"MODELS?"))snprintf(data,sizeof(data),"{\"data\":[{\"id\":4,\"address\":%u,\"size\":0},{\"id\":3,\"address\":8388608,\"size\":0},{\"id\":2,\"address\":6291456,\"size\":0},{\"id\":1,\"address\":4194304,\"size\":0}]}",!strcmp(mode,"bad-layout")?10485504:10485760);
    else if(!strcmp(name,"BREAK"))strcpy(data,"{\"data\":{}}");
    else if(!strcmp(name,"INVOKE")) {
        assert(event&&sensor&&selected==4);
        if(!strcmp(mode,"inference-failure"))return SOLAR_OS_NATIVE_ERROR_FAILED;
        strcpy(data,!strcmp(mode,"wrong-result")?"{\"data\":{\"image\":\"unused\"}}":
            !strcmp(mode,"malformed-result")?"{\"data\":{\"classes\":[garbage]}}":"{\"data\":{\"classes\":[[90,1]],\"boxes\":[],\"points\":[],\"perf\":[1,2,3]}}");
    } else {assert(0);return -1;}
    *reply=provider_alloc(strlen(data)+1);
    if(!*reply)return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    strcpy(*reply,data);return 0;
}
static int send_camera(uint32_t token,const char *source,const char *command)
{assert(token==77&&owner&&!ota&&!strcmp(source,"camera0")&&!strcmp(command,"AT+OTA\r\n"));ota=true;return 0;}
static int reset_camera(uint32_t token)
{
    assert(token==77&&owner);ota=false;sensor=false;resets++;
    if(!strcmp(mode,"reset-default"))selected=4;
    return ((!strcmp(mode,"reset-failure")&&resets==1)||(!strcmp(mode,"restore-failure")&&resets==3))?SOLAR_OS_NATIVE_ERROR_FAILED:0;
}
static int spi_camera(uint32_t token,const void *data,void *output,size_t n)
{
    assert(token==77&&owner);
    if(!n){assert(!data&&!output);probes++;return !strcmp(mode,"missing-spi")?SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED:0;}
    assert(ota&&data&&n<=262&&selected==1);raw_calls++;
    const uint8_t *tx=data;
    if(!strcmp(mode,"spi-error")&&raw_calls==8)return SOLAR_OS_NATIVE_ERROR_FAILED;
    if(tx[0]==0xf2&&tx[1]==0) {
        assert(n==6||n==10||n==262);target_address=get32(tx+2);
        if(n==10) {assert(target_address==0x51010000&&get32(tx+6)==0x028c208b);pages=0;}
        else if(n==6) assert(target_address==0x51010040);
        else {
            assert(target_address>=0x38a00000&&target_address+256<=0x38c00000);
            size_t offset=target_address-0x38a00000;assert(offset==programmed_size);
            memcpy(programmed+offset,tx+6,256);programmed_size+=256;pages++;
        }
    } else if(tx[0]==0xf2) {
        assert(n==3);
        assert((tx[1]==0xd8&&(tx[2]==7||tx[2]==0))||(tx[1]==0x13&&tx[2]==0x31)||
            (tx[1]==0x0d&&tx[2]==0x11)||(tx[1]==0x0c&&tx[2]==0));
    } else {
        assert(tx[0]==0xf3&&tx[1]==8&&n==7&&output&&target_address==0x51010040);
        memset(output,0,n);uint32_t status=pages;
        if(!strcmp(mode,"crc-error"))status|=0x10000000;
        if(!strcmp(mode,"crc-state3"))status|=0x30000000;
        if(!strcmp(mode,"timeout"))status=0;
        set32((uint8_t *)output+3,status);
    }
    return 0;
}
static bool stop_callback(void *unused)
{(void)unused;return !strcmp(mode,"cancel")&&pages>=2;}
static void progress_callback(const char *stage,uint32_t n,uint32_t total,void *unused)
{(void)unused;assert(stage&&n<=total);}
static int64_t now(void){clock_us+=1000;return clock_us;}
static void sleep_time(uint32_t ms){clock_us+=(uint64_t)ms*1000;slept++;}
static const solar_os_native_memory_api_v1_t memory_api={.abi_version=1,.struct_size=sizeof(memory_api),.alloc=provider_alloc,.free=solar_os_memory_free};
static const solar_os_native_libc_api_v1_t libc_api={.abi_version=1,.struct_size=sizeof(libc_api),.format=vsnprintf};
static const solar_os_native_time_api_v1_t time_api={1,sizeof(time_api),now,sleep_time};
static const solar_os_native_sscma_api_v1_t camera_api={.abi_version=1,.struct_size=sizeof(camera_api),.open=open_camera,.start=start_camera,
    .exchange=exchange_camera,.close=close_camera,.spi=spi_camera,.send=send_camera,.reset=reset_camera};
static const void *services(const char *name,uint32_t abi,uint32_t size)
{
    assert(abi==1);
    if(!strcmp(name,"memory")){assert(size<=sizeof(memory_api));return &memory_api;}
    if(!strcmp(name,"libc")){assert(size<=sizeof(libc_api));return &libc_api;}
    if(!strcmp(name,"time")){assert(size<=sizeof(time_api));return &time_api;}
    if(!strcmp(name,"camera.sscma")){assert(size<=sizeof(camera_api));return &camera_api;}
    if(!strcmp(name,SOLAR_OS_NATIVE_FILES_SERVICE)&&!strcmp(mode,"slots-old-host")) {
        static solar_os_native_files_api_v1_t legacy;
        legacy=*(const solar_os_native_files_api_v1_t *)solar_os_native_data_get_service(name,abi,SOLAR_OS_NATIVE_FILES_BASE_SIZE);
        legacy.struct_size=SOLAR_OS_NATIVE_FILES_BASE_SIZE;legacy.state_directory=NULL;
        return size<=legacy.struct_size?&legacy:NULL;
    }
    return solar_os_native_data_get_service(name,abi,size);
}
int main(int argc,char **argv)
{
    assert(argc==4);mount_root=argv[1];mode=argv[3];char *reply=NULL;
    const solar_os_native_files_api_v1_t *files=solar_os_native_data_get_service(SOLAR_OS_NATIVE_FILES_SERVICE,1,SOLAR_OS_NATIVE_FILES_BASE_SIZE);
    assert(files&&files->struct_size>=sizeof(*files));
    char directory[160];
    assert(files->state_directory("../bad",directory,sizeof(directory))==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(files->state_directory("himax",directory,2)==(!solar_os_storage_flash_is_mounted()?SOLAR_OS_NATIVE_ERROR_NOT_FOUND:SOLAR_OS_NATIVE_ERROR_INVALID_SIZE));
    if(!strncmp(mode,"oom-",4))no_memory_after=atoi(mode+4);
    if(!strncmp(mode,"slots-oom-",10))no_memory_after=atoi(mode+10);
    int error;
    if(!strncmp(mode,"slots",5)) {
        if(!strcmp(mode,"slots-active"))selected=2;
        error=solar_os_himax_model_slots(services,!strcmp(mode,"slots-alias")?"camera1":"camera0",1000,&reply);
        assert(!raw_calls);
        if(!strncmp(mode,"slots-oom-",10)&&error)printf("{\"error\":%d}\n",error);
        else {assert(!error&&reply);puts(reply);}
        solar_os_memory_free(reply);assert(!owner&&!allocations);return 0;
    } else if(!strcmp(mode,"select")) {
        error=solar_os_himax_model_select(services,"camera0",4,1000,stop_callback,NULL,&reply);
        assert(!error&&selected==4&&!raw_calls&&reply);
        solar_os_memory_free(reply);reply=NULL;
        error=solar_os_himax_model_select(services,"camera0",5,1000,NULL,NULL,&reply);
    } else {
        uint32_t slot=!strcmp(mode,"protected-slot")?1:!strcmp(mode,"active-slot")?selected:4;
        uint32_t flags=!strcmp(mode,"no-replace")?0:SOLAR_OS_PROCESSOR_UPLOAD_REPLACE;
        error=solar_os_himax_model_upload(services,"camera0",argv[2],slot,flags,1000,
            stop_callback,progress_callback,NULL,&reply);
        if(!error) {
            assert(selected==1&&pages&&resets==3&&reply);
            char path[512];snprintf(path,sizeof(path),"%s/programmed.bin",mount_root);
            FILE *f=fopen(path,"wb");assert(f);assert(fwrite(programmed,1,programmed_size,f)==programmed_size);fclose(f);
        }
    }
    printf("{\"error\":%d,\"selected\":%u,\"pages\":%u,\"resets\":%u,\"probes\":%u,\"raw_calls\":%u}\n",error,selected,pages,resets,probes,raw_calls);
    solar_os_memory_free(reply);assert(!owner&&!allocations);return 0;
}
