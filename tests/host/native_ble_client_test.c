#include "solar_os_native_ble.h"
#include "solar_os_native_ble_abi.h"
#include "solar_os_memory.h"
#define main baseline_ble_checks
#include "ble_multi_peer_test.c"
#undef main

static unsigned adapter_allocations;
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    assert(kind==SOLAR_OS_MEMORY_TRANSIENT && !strncmp(tag,"native.ble.",11));
    if(fail_allocation)return NULL;
    void *value=calloc(n,size);assert(value);++adapter_allocations;return value;
}
void solar_os_memory_free(void *value) { if(value){assert(adapter_allocations);--adapter_allocations;free(value);} }
static const solar_os_native_ble_client_api_v1_t *api;
typedef struct { solar_os_native_ble_session_t session;solar_os_native_ble_peer_t peer;int result; } native_call_t;
static void *native_read(void *value)
{
    native_call_t *call=value;uint8_t byte;size_t length;
    call->result=api->peer_read(call->session,call->peer,3,&byte,1,&length,5000);return NULL;
}
int native_ble_client_checks(void)
{
    baseline_ble_checks();assert(!entries());
    api=solar_os_native_ble_get_service("ble.client",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(api->max_value_size==SOLAR_OS_NATIVE_BLE_VALUE_MAX);
    assert(!solar_os_native_ble_get_service("ble.client",2,8));
    assert(!solar_os_native_ble_get_service("ble.client",1,sizeof(*api)+1));
    assert(!solar_os_native_ble_get_service("missing",1,8));
    assert(!api->init() && api->peer_capacity()==3);
    sample_scan=true;solar_os_native_ble_scan_result_v1_t scanned[2];size_t count=999;
    assert(!api->scan(scanned,2,&count) && count==2 && !adapter_allocations);
    assert(scanned[0].bda[5]==6 && scanned[0].addr_type==1 && scanned[0].rssi==-37 && scanned[0].appearance==0x3c1);
    assert(scanned[0].hid_service && scanned[0].keyboard_like && scanned[0].remembered && !scanned[0].connected);
    assert(!strcmp(scanned[0].name,"Keyboard") && !strcmp(scanned[1].name,"Sensor") && !scanned[1].keyboard_like);
    assert(api->scan(NULL,1,&count)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT);
    assert(api->scan(scanned,SIZE_MAX,&count)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT);
    fail_allocation=true;assert(api->scan(scanned,1,&count)==SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY && !adapter_allocations);fail_allocation=false;
    solar_os_native_ble_session_t owner,other;
    assert(!api->session_create("native",&owner) && !api->session_create("foreign",&other));
    uint8_t bda[6]={1,2,3,4,5,1};solar_os_native_ble_peer_t a,b,c,extra;
    fail_allocation=true;assert(api->peer_connect(owner,bda,0,100,&a)==SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY && !a);fail_allocation=false;
    assert(!api->peer_connect(owner,bda,0,100,&a));
    bda[5]=2;assert(!api->peer_connect(owner,bda,0,100,&b));
    bda[5]=3;assert(!api->peer_connect(other,bda,0,100,&c));
    assert(api->peer_connect(owner,bda,0,100,&extra)==SOLAR_OS_NATIVE_BLE_ERROR_CAPACITY && !extra);
    solar_os_native_ble_peer_info_v1_t info;
    assert(api->peer_get_info(other,a,&info)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE);
    assert(api->peer_pair(owner,a,1000000,100)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT);
    assert(!api->peer_pair(owner,a,123456,100));
    assert(!api->peer_get_info(owner,a,&info) && !strcmp(info.owner,"native") && info.gatt.connected && info.gatt.encrypted && info.gatt.bonded && info.gatt.mtu==247);
    assert(info.gatt.bda[5]==1 && info.gatt.service_count==1 && !strcmp(info.gatt.status,"secured"));
    solar_os_native_ble_service_v1_t service;
    assert(!api->peer_services(owner,b,&service,1,&count) && count==1 && !adapter_allocations && !strcmp(service.uuid,"peer-2") && service.start_handle==1 && service.end_handle==9);
    solar_os_native_ble_characteristic_v1_t characteristic;
    assert(!api->peer_characteristics(owner,b,0,&characteristic,1,&count) && count==1 && characteristic.handle==3 && characteristic.properties==0x12 && !strcmp(characteristic.uuid,"2a19"));
    fail_allocation=true;
    assert(api->peer_services(owner,b,&service,1,&count)==SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY);
    assert(api->peer_characteristics(owner,b,0,&characteristic,1,&count)==SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY);fail_allocation=false;
    assert(api->peer_services(owner,b,&service,SIZE_MAX,&count)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT && !adapter_allocations);
    assert(!api->peer_configure_queue(owner,a,2));
    assert(!api->peer_subscribe(owner,a,3,SOLAR_OS_NATIVE_BLE_SUBSCRIBE_NOTIFY,100));
    notify_peer(a,3,false,2);notify_peer(a,3,true,0);notify_peer(a,3,false,2);
    solar_os_native_ble_notification_v1_t notification;
    assert(!api->peer_get_info(owner,a,&info) && info.event_count==2 && info.events_dropped==1);
    assert(!api->peer_poll(owner,a,&notification) && notification.value_len==2 && notification.value[0]==0 && notification.value[1]==255 && !notification.indication);
    assert(!api->peer_poll(owner,a,&notification) && !notification.value_len && notification.indication);
    assert(api->peer_poll(owner,a,&notification)==SOLAR_OS_NATIVE_BLE_ERROR_NOT_FOUND);
    uint8_t byte;size_t length;
    assert(!api->peer_read(owner,b,3,&byte,1,&length,100) && byte==8 && length==1);
    assert(!api->peer_write(owner,b,3,&byte,1,true,100));
    assert(api->peer_read(other,b,3,&byte,1,&length,100)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE);
    atomic_store(&read_request,0);atomic_store(&delayed_epoch,find_locked(a)->link.epoch);
    native_call_t call={.session=owner,.peer=a};pthread_t worker;assert(!pthread_create(&worker,NULL,native_read,&call));
    for(unsigned i=0;!atomic_load(&read_request) && i<2000;++i){struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}
    assert(atomic_load(&read_request));
    assert(!api->peer_read(other,c,3,&byte,1,&length,100) && byte==9);
    assert(!api->peer_disconnect(owner,a));assert(!pthread_join(worker,NULL) && call.result==SOLAR_OS_NATIVE_BLE_ERROR_CANCELLED);
    atomic_store(&delayed_epoch,0);
    assert(!api->session_cancel(owner));
    assert(api->peer_read(owner,b,3,&byte,1,&length,100)==SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE);
    assert(!api->session_close(owner));
    assert(!api->peer_read(other,c,3,&byte,1,&length,100) && byte==9);
    assert(!api->session_close(other) && !entries() && !adapter_allocations);
    puts("native BLE client: real sessions, peer isolation, scan/value copies, OOM cleanup, subscriptions, cancellation and independent I/O passed");
    return 0;
}
#ifndef SOLAR_OS_NATIVE_BLE_CLIENT_FIXTURE
int main(void) { return native_ble_client_checks(); }
#endif
