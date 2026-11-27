#include "solar_os_native_ble.h"
#include "solar_os_native_bluetooth_abi.h"
#include "solar_os_memory.h"
#include <stdlib.h>
#define main baseline_ble_service_checks
#include "ble_service_test.c"
#undef main

static unsigned adapter_allocations;
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{ (void)kind;(void)tag;++adapter_allocations;return calloc(n,size); }
void solar_os_memory_free(void *value) { free(value); }
static const solar_os_native_bluetooth_api_v1_t *api;
typedef struct { solar_os_ble_session_t owner;solar_os_ble_peer_t peer;int result; } modern_read_t;
static void *modern_read(void *value)
{
    modern_read_t *call=value;uint8_t bytes[2];size_t count;
    call->result=api->peer_read(call->owner,call->peer,3,bytes,sizeof(bytes),&count,5000);return NULL;
}
static void check_peers(void)
{
    solar_os_ble_session_t owner,other;solar_os_ble_peer_t connected,extra;
    assert(!api->session_create("modern-peers",&owner));assert(!api->session_create("foreign",&other));
    submit_result=SOLAR_OS_BLE_ERR_CAPACITY;
    assert(api->peer_connect(owner,peer,SOLAR_OS_BLE_ADDR_RANDOM,100,&connected)==SOLAR_OS_NATIVE_ERROR_NO_SPACE && !connected);
    submit_result=ESP_ERR_NO_MEM;
    assert(api->peer_connect(owner,peer,SOLAR_OS_BLE_ADDR_RANDOM,100,&connected)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !connected);submit_result=ESP_OK;
    assert(!api->peer_connect(owner,peer,SOLAR_OS_BLE_ADDR_RANDOM,100,&connected));
    assert(api->peer_connect(other,peer,SOLAR_OS_BLE_ADDR_RANDOM,100,&extra)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !extra);
    solar_os_ble_session_info_t info;
    assert(api->peer_get_info(other,connected,&info)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->peer_get_info(owner,connected,&info) && info.gatt.connected);
    assert(!api->peer_pair(owner,connected,123456,100));
    solar_os_ble_gatt_service_t service;solar_os_ble_gatt_characteristic_t characteristic;size_t count;
    assert(!api->peer_services(owner,connected,&service,1,&count) && !strcmp(service.uuid,"0x180f"));
    assert(!api->peer_characteristics(owner,connected,0,&characteristic,1,&count) && characteristic.handle==3);
    uint8_t bytes[2];assert(!api->peer_read(owner,connected,3,bytes,sizeof(bytes),&count,100) && bytes[0]==0x42);
    bytes[0]=0;bytes[1]=255;assert(!api->peer_write(owner,connected,3,bytes,2,true,100));
    assert(!api->peer_configure_queue(owner,connected,2));
    assert(!api->peer_subscribe(owner,connected,3,SOLAR_OS_NATIVE_BLE_SUBSCRIBE_NOTIFY,100));
    const uint8_t payload[]={0,255};
    solar_os_ble_backend_event_t input={.type=SOLAR_OS_BLE_BACKEND_NOTIFICATION,.epoch=fake_epoch,
        .conn_id=7,.handle=3,.value=payload,.value_len=sizeof(payload)};
    solar_os_ble_service_event(&input);
    solar_os_ble_notification_t notification;
    assert(!api->peer_poll(owner,connected,&notification) && notification.value_len==2 && notification.value[0]==0 && notification.value[1]==255);
    assert(api->peer_poll(owner,connected,&notification)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    defer_read=true;unsigned before=submission_count();modern_read_t call={.owner=owner,.peer=connected};pthread_t worker;
    assert(!pthread_create(&worker,NULL,modern_read,&call));(void)await_submission(before);
    assert(!api->session_cancel(owner));assert(!pthread_join(worker,NULL) && call.result==SOLAR_OS_NATIVE_ERROR_CANCELLED);
    retired();defer_read=false;
    assert(!api->peer_disconnect(owner,connected));
    assert(!api->session_close(owner));assert(!api->session_close(other));
}
static void check_default_clients(void)
{
    defer_read=defer_write=defer_connect=defer_subscription=false;submit_result=ESP_OK;
    solar_os_ble_scan_result_t scan;size_t count;
    assert(!api->scan(&scan,1,&count) && count==1 && !strcmp(scan.name,"Sensor") && scan.rssi==-73 && scan.appearance==961);
    uint8_t bda[6];assert(api->parse_address("01:02:03:04:05:06",17,bda) && !memcmp(bda,peer,6));
    assert(!api->parse_address("01:02:03:04:05:xx",17,bda));
    solar_os_ble_session_t owner;char label[]="modern";
    assert(!api->session_create(label,&owner));label[0]='x';
    assert(!api->session_connect(owner,peer,SOLAR_OS_BLE_ADDR_RANDOM,100));
    assert(!api->session_pair(owner,123456,100));
    solar_os_ble_session_info_t info;assert(!api->session_get_info(owner,&info) && !strcmp(info.owner,"modern") && info.gatt.connected && info.gatt.encrypted && info.gatt.bonded);
    solar_os_ble_gatt_service_t service;
    assert(!api->session_services(owner,&service,1,&count) && count==SOLAR_OS_BLE_GATT_MAX_SERVICES && service.primary && !strcmp(service.uuid,"0x180f"));
    solar_os_ble_gatt_characteristic_t characteristic;
    assert(!api->session_characteristics(owner,0,&characteristic,1,&count) && count==1 && characteristic.handle==3 && !strcmp(characteristic.uuid,"0x2a19"));
    uint8_t bytes[2];assert(!api->session_read(owner,3,bytes,sizeof(bytes),&count,100) && bytes[0]==0x42 && count==SOLAR_OS_BLE_GATT_VALUE_MAX);
    bytes[0]=0;bytes[1]=255;assert(!api->session_write(owner,3,bytes,2,true,100) && write_response);
    assert(!api->session_close(owner));retired();
    assert(api->session_get_info(owner,&info)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->gatt_connect(peer,SOLAR_OS_BLE_ADDR_RANDOM,100));
    assert(!api->gatt_pair(123456,100));
    solar_os_ble_gatt_status_t status;api->gatt_get_status(&status);assert(status.connected && status.encrypted && status.mtu==247);
    assert(!api->gatt_services(&service,1,&count) && count==SOLAR_OS_BLE_GATT_MAX_SERVICES);
    assert(!api->gatt_characteristics(0,&characteristic,1,&count) && characteristic.handle==3);
    assert(!api->gatt_read(3,bytes,sizeof(bytes),&count,100) && bytes[0]==0x42);
    bytes[0]=0;bytes[1]=255;assert(!api->gatt_write(3,bytes,2,false,100) && !write_response);
    assert(!api->gatt_disconnect());retired();api->gatt_get_status(&status);assert(!status.connected);
}
static void check_server_and_hid(void)
{
    solar_os_ble_session_t owner,other;assert(!api->session_create("peripheral",&owner));assert(!api->session_create("foreign",&other));
    solar_os_ble_server_request_t request={.op=SOLAR_OS_BLE_SERVER_CREATE};strcpy(request.text,"SDK server");
    assert(!api->server_request(owner,&request) && fake_server_owner==owner);
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SERVICE};strcpy(request.text,"180f");
    assert(!api->server_request(owner,&request) && request.id);uint32_t service=request.id;
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC,.parent=service,.properties=SOLAR_OS_BLE_CHAR_READ};strcpy(request.text,"2a19");
    assert(!api->server_request(owner,&request) && request.id);
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SET,.id=request.id,.value_len=2,.value={0,255}};
    assert(!api->server_request(owner,&request) && fake_server_request.value_len==2 && fake_server_request.value[1]==255);
    memset(request.value,0,sizeof(request.value));assert(fake_server_request.value[1]==255);
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(!api->server_request(owner,&request) && request.info.event_capacity==16);
    assert(api->server_request(other,&request)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    request.value_len=SOLAR_OS_BLE_GATT_VALUE_MAX+1;
    assert(api->server_request(owner,&request)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CLOSE};assert(!api->server_request(owner,&request));
    assert(!api->hid_device_start(owner,"SDK HID") && !fake_hid_request.manual);
    assert(!api->hid_device_stop(owner));
    assert(!api->hid_device_start_managed(owner,"SDK HID") && fake_hid_request.manual);
    solar_os_ble_hid_host_t hosts[2];size_t count;
    assert(!api->hid_device_hosts(owner,hosts,2,&count) && count==1 && hosts[0].connected && hosts[0].bda[5]==6 && !strcmp(hosts[0].name,"Desktop"));
    assert(!api->hid_device_connect(owner,hosts[0].bda,hosts[0].addr_type) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_CONNECT && !memcmp(fake_hid_request.bda,hosts[0].bda,6));
    assert(!api->hid_device_forget(owner,hosts[0].bda,hosts[0].addr_type) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_FORGET);
    assert(!api->hid_device_disconnect(owner) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_DISCONNECT);
    assert(!api->hid_device_pair(owner) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_PAIR);
    solar_os_ble_hid_info_t info;assert(!api->hid_device_status(owner,&info) && info.event_capacity==16 && !strcmp(info.host_name,"Desktop"));
    fake_hid_event=(solar_os_ble_hid_device_event_t){.type=SOLAR_OS_BLE_HID_KEYBOARD_LEDS,.keyboard_leds=SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK};fake_hid_event_ready=true;
    solar_os_ble_hid_device_event_t event;assert(!api->hid_device_poll(owner,&event) && event.type==SOLAR_OS_BLE_HID_KEYBOARD_LEDS && event.keyboard_leds==SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK);
    uint16_t keys[]={4,0xe1};
    assert(!api->hid_keyboard_press(owner,keys,2) && fake_hid_request.key_count==2 && fake_hid_request.keys[0]==4);
    assert(!api->hid_keyboard_release(owner,keys,2) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE);
    assert(!api->hid_keyboard_release_all(owner) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE_ALL);
    assert(!api->hid_mouse_move(owner,12,-3) && fake_hid_request.x==12 && fake_hid_request.y==-3);
    assert(!api->hid_mouse_button(owner,1,true) && fake_hid_request.button==1 && fake_hid_request.pressed);
    assert(!api->hid_gamepad_axis(owner,2,-123) && fake_hid_request.axis==2 && fake_hid_request.value==-123);
    assert(!api->hid_gamepad_button(owner,3,true) && fake_hid_request.button==3 && fake_hid_request.pressed);
    assert(!api->hid_gamepad_hat(owner,7) && fake_hid_request.hat==7);
    assert(!api->hid_gamepad_send(owner) && fake_hid_request.op==SOLAR_OS_BLE_HID_OP_GAMEPAD_SEND);
    assert(api->hid_device_status(other,&info)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->hid_device_stop(owner));assert(!api->session_close(owner));assert(!api->session_close(other));
}
int main(void)
{
    assert(!baseline_ble_service_checks());
    api=solar_os_native_ble_get_service("ble",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_ble_get_service("ble",2,8));
    assert(!solar_os_native_ble_get_service("ble",1,sizeof(*api)+1));
    solar_os_native_bluetooth_limits_v1_t limits;
    assert(!api->limits(&limits) && limits.value_bytes==176 && limits.gatt_services==24 && limits.gatt_characteristics==64 && limits.scan_results==32 && limits.owner_bytes==32 && limits.hid_hosts==8);
    assert(api->limits(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && api->peer_capacity()==1);
    assert(!api->init());check_default_clients();check_peers();check_server_and_hid();
    assert(!adapter_allocations); /* Canonical records need no bridge buffers. */
    puts("Bluetooth ABI: canonical values, normalized results, session/default clients, shared GATT, server records and managed HID controls passed without adapter allocations");
    return 0;
}
