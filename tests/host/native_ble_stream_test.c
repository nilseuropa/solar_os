#include "solar_os_native_ble_stream.h"
#include "solar_os_native_ble_stream_abi.h"
#include "solar_os_ble_stream.h"
#include "solar_os_native_support.h"
static const solar_os_native_ble_stream_api_v1_t *api;
/* Replay the real registry/port/stream corpus through normalized ABI calls. */
static esp_err_t abi_listen(const char *name)
{ return solar_os_native_callback_result(api->listen(name)); }
static esp_err_t abi_connect(const char *name,const uint8_t address[6],uint8_t type)
{ return solar_os_native_callback_result(api->connect(name,address,type)); }
static esp_err_t abi_remove(const char *name)
{ return solar_os_native_callback_result(api->remove(name)); }
static esp_err_t abi_status(const char *name,solar_os_ble_stream_info_t *out)
{ return solar_os_native_callback_result(api->status(name,out)); }
#define solar_os_ble_stream_listen abi_listen
#define solar_os_ble_stream_connect abi_connect
#define solar_os_ble_stream_remove abi_remove
#define solar_os_ble_stream_status abi_status
#define main stream_registry_checks
#include "ble_stream_test.c"
#undef main
#undef solar_os_ble_stream_listen
#undef solar_os_ble_stream_connect
#undef solar_os_ble_stream_remove
#undef solar_os_ble_stream_status
int main(void)
{
    api=solar_os_native_ble_stream_get_service("stream.ble",1,sizeof(*api));
    assert(api && api->abi_version==1 && api->struct_size==sizeof(*api));
    assert(!solar_os_native_ble_stream_get_service("stream.ble",2,8));
    assert(!solar_os_native_ble_stream_get_service("stream.ble",1,sizeof(*api)+1));
    assert(!solar_os_native_ble_stream_get_service("ble",1,8));
    solar_os_native_ble_stream_limits_v1_t limits;
    assert(api->limits(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->limits(&limits) && limits.maximum_endpoints && limits.name_bytes>1);
    assert(!stream_registry_checks());
    solar_os_ble_stream_info_t info={.claimed=true,.connected=true};
    assert(api->status("missing",&info)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.claimed && !info.connected);
    assert(api->status("missing",NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->listen("")==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->connect("bad",NULL,0)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    uint8_t address[6]={1,2,3,4,5,6};
    assert(api->connect("bad",address,4)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    for(uint32_t i=0;i<limits.maximum_endpoints;++i) {
        char name[16];snprintf(name,sizeof(name),"abi-%u",i);assert(!api->listen(name));
    }
    assert(api->listen("overflow")==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    assert(api->listen("abi-0")==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    for(uint32_t i=0;i<limits.maximum_endpoints;++i) {
        char name[16];snprintf(name,sizeof(name),"abi-%u",i);assert(!api->remove(name));
    }
    assert(api->remove("abi-0")==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    puts("Bluetooth stream ABI: real named registry, exclusive claims, retirement, rollback, status copies and limits passed");
    return 0;
}
