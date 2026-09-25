#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "nimble_test_support.h"
#include "../../src/services/solar_os_ble_hid.c"

esp_err_t solar_os_ble_nimble_error(int status) { return status ? ESP_FAIL : ESP_OK; }
void solar_os_ble_nimble_address(ble_addr_t *out,const uint8_t bda[6],uint8_t type)
{ out->type=type; for(size_t i=0;i<6;++i)out->val[i]=bda[5-i]; }
void solar_os_ble_nimble_display_address(uint8_t out[6], const ble_addr_t *addr)
{ for(size_t i=0;i<6;++i)out[i]=addr->val[5-i]; }
int solar_os_ble_nimble_security(struct ble_gap_event *e) { (void)e; return 0; }
int solar_os_ble_nimble_security_passkey(struct ble_gap_event *e, uint32_t *passkey)
{ (void)e; *passkey = 12345; return 0; }
static struct ble_gatt_error ok, done={.status=BLE_HS_EDONE};
static uint32_t epoch;
static void start(void)
{
    assert(solar_os_ble_hid_idle());
    nimble_test_reset();
    memset(&hid,0,sizeof(hid)); memset(&device,0,sizeof(device));
    device.conn_id=-1; hid.active=true; hid.epoch=++epoch;
    submit_command(); nimble_test_drain();
    assert(fake.connect_calls==1);
}
static void discover(void)
{
    start(); nimble_test_connect(0);
    struct ble_gap_event encrypted={.type=BLE_GAP_EVENT_ENC_CHANGE};
    fake.gap(&encrypted,fake.gap_arg);
    fake.mtu_fn(7,&ok,517,fake.arg);
}
static void drain_policy(void)
{
    hid_event_t e;
    while(xQueueReceive(events,&e,0)==pdTRUE)if(e.type==SOLAR_OS_BLE_HID_CLOSE)release();
    nimble_test_drain();
}
static void close_peer(void)
{
    solar_os_ble_hid_close(&device); nimble_test_drain();
    nimble_test_disconnect(); drain_policy();
    assert(solar_os_ble_hid_idle());
}
int main(void)
{
    assert(solar_os_ble_hid_init(NULL)==ESP_OK);
    /* Cancelling before submission does not touch a generic client's attempt. */
    hid.active=true; hid.epoch=++epoch; device.conn_id=-1;
    solar_os_ble_hid_cancel_open(); nimble_test_drain();
    assert(!fake.connect_calls && !fake.cancel_calls && solar_os_ble_hid_idle());
    start(); hid.caller_waiting=true;
    nimble_test_connect(BLE_HS_ETIMEOUT); nimble_test_drain();
    assert(!solar_os_ble_hid_idle());
    hid.caller_waiting=false; submit_command(); nimble_test_drain();
    assert(solar_os_ble_hid_idle());
    start(); nimble_test_connect(BLE_HS_ETIMEOUT); nimble_test_drain();
    assert(solar_os_ble_hid_idle()); /* Failed connect has no device lookup/deref. */
    start(); nimble_test_connect(0);
    struct ble_gap_event repeat_pairing={.type=BLE_GAP_EVENT_REPEAT_PAIRING,
        .repeat_pairing={.conn_handle=7}};
    assert(fake.gap(&repeat_pairing,fake.gap_arg)==BLE_GAP_REPEAT_PAIRING_RETRY);
    assert(fake.store_delete_calls==1);
    fake.store_delete_error=BLE_HS_EAPP;
    assert(fake.gap(&repeat_pairing,fake.gap_arg)==BLE_GAP_REPEAT_PAIRING_IGNORE);
    assert(fake.store_delete_calls==2);
    solar_os_ble_hid_cancel_open(); nimble_test_drain();
    nimble_test_disconnect(); drain_policy();
    assert(solar_os_ble_hid_idle());
    start(); solar_os_ble_hid_cancel_open(); nimble_test_drain();
    assert(fake.cancel_calls==1); nimble_test_connect(BLE_HS_EAPP); nimble_test_drain();
    assert(solar_os_ble_hid_idle());
    discover();
    struct ble_gatt_svc service={.start_handle=1,.end_handle=80,.uuid.u16={{16},0x1812}};
    for(size_t i=0;i<HID_SERVICE_MAX;++i)fake.svc(7,&ok,&service,fake.arg);
    assert(fake.svc(7,&ok,&service,fake.arg)==BLE_HS_ENOMEM);
    assert(hid.closing && fake.terminate_calls==1); nimble_test_disconnect(); nimble_test_drain();
    discover();
    fake.svc(7,&ok,&service,fake.arg);
    struct ble_gatt_svc battery_service={.start_handle=81,.end_handle=90,.uuid.u16={{16},0x180f}};
    fake.svc(7,&ok,&battery_service,fake.arg);
    fake.svc(7,&done,NULL,fake.arg);
    struct ble_gatt_chr chr={.def_handle=2,.val_handle=3,.uuid.u16={{16},0x2a4b}};
    for(size_t i=0;i<HID_CHAR_MAX;++i)fake.chr(7,&ok,&chr,fake.arg);
    assert(fake.chr(7,&ok,&chr,fake.arg)==BLE_HS_ENOMEM);
    nimble_test_disconnect(); nimble_test_drain();

    discover();
    fake.svc(7,&ok,&service,fake.arg);
    fake.svc(7,&ok,&battery_service,fake.arg);
    fake.svc(7,&done,NULL,fake.arg);
    fake.chr(7,&ok,&chr,fake.arg);
    chr=(struct ble_gatt_chr){.def_handle=4,.val_handle=5,.properties=0x10,.uuid.u16={{16},0x2a4d}};
    fake.chr(7,&ok,&chr,fake.arg);
    chr=(struct ble_gatt_chr){.def_handle=8,.val_handle=9,.properties=BLE_GATT_CHR_PROP_WRITE_NO_RSP,.uuid.u16={{16},0x2a4c}};
    fake.chr(7,&ok,&chr,fake.arg);
    chr=(struct ble_gatt_chr){.def_handle=10,.val_handle=11,.properties=BLE_GATT_CHR_PROP_READ,.uuid.u16={{16},0x2a4a}};
    fake.chr(7,&ok,&chr,fake.arg); fake.chr(7,&done,NULL,fake.arg);
    uint8_t map[]={0x05,1,0x09,6,0xa1,1,0x85,1,0x81,2,0xc0};
    nimble_test_value(0,3,map,sizeof(map)); nimble_test_value(BLE_HS_EDONE,3,NULL,0);
    assert(fake.last_start==5 && fake.last_end==7);
    struct ble_gatt_dsc dsc={.handle=6,.uuid.u16={{16},0x2902}};
    fake.dsc(7,&ok,5,&dsc,fake.arg);
    dsc.handle=7; dsc.uuid.u16.value=0x2908; fake.dsc(7,&ok,5,&dsc,fake.arg);
    fake.dsc(7,&done,5,NULL,fake.arg);
    uint8_t ref[]={1,1}; nimble_test_value(0,7,ref,2);
    assert(fake.last_handle==6 && fake.written_len==2 && fake.written[0]==1);
    nimble_test_value(0,6,NULL,0);
    chr=(struct ble_gatt_chr){.def_handle=82,.val_handle=83,
        .properties=BLE_GATT_CHR_PROP_READ|BLE_GATT_CHR_PROP_NOTIFY,
        .uuid.u16={{16},0x2a19}};
    fake.chr(7,&ok,&chr,fake.arg); fake.chr(7,&done,NULL,fake.arg);
    dsc=(struct ble_gatt_dsc){.handle=84,.uuid.u16={{16},0x2902}};
    fake.dsc(7,&ok,83,&dsc,fake.arg); fake.dsc(7,&done,83,NULL,fake.arg);
    assert(fake.last_handle==84 && fake.written_len==2 && fake.written[0]==1);
    nimble_test_value(0,84,NULL,0);
    assert(fake.last_handle==83);
    uint8_t initial_battery=87;
    nimble_test_value(0,83,&initial_battery,1);
    assert(device.connected && hid.open_sent && !deadline.active);
    solar_os_ble_hid_keepalive_status_t keepalive_status;
    solar_os_ble_hid_get_keepalive_status(&keepalive_status);
    assert(keepalive_status.method==SOLAR_OS_BLE_HID_KEEPALIVE_EXIT_SUSPEND);
    assert(!keepalive_status.attempted && !keepalive_status.pending);
    int keepalive_writes=fake.write_calls;
    assert(solar_os_ble_hid_keepalive(&device)==ESP_OK);
    assert(solar_os_ble_hid_keepalive(&device)==ESP_ERR_NOT_FINISHED);
    nimble_test_drain();
    assert(fake.write_calls==keepalive_writes+1 && fake.last_handle==9);
    assert(fake.written_len==1 && fake.written[0]==1);
    solar_os_ble_hid_get_keepalive_status(&keepalive_status);
    assert(keepalive_status.attempts==1 && keepalive_status.attempted);
    assert(!keepalive_status.pending && keepalive_status.last_status==ESP_OK);

    /* Fall back to a read when the peer does not expose HID Control Point. */
    hid.keepalive_control_count=0;
    int keepalive_reads=fake.read_calls;
    assert(solar_os_ble_hid_keepalive(&device)==ESP_OK);
    assert(solar_os_ble_hid_keepalive(&device)==ESP_ERR_NOT_FINISHED);
    nimble_test_drain();
    assert(fake.read_calls==keepalive_reads+1 && fake.last_handle==11);
    assert(solar_os_ble_hid_keepalive(&device)==ESP_ERR_NOT_FINISHED);
    uint8_t hid_info[]={0x11,0x01,0x00,0x02};
    nimble_test_value(0,11,hid_info,sizeof(hid_info));
    solar_os_ble_hid_get_keepalive_status(&keepalive_status);
    assert(keepalive_status.method==SOLAR_OS_BLE_HID_KEEPALIVE_INFORMATION_READ);
    assert(keepalive_status.attempts==2 && keepalive_status.last_status==ESP_OK);
    assert(solar_os_ble_hid_keepalive(&device)==ESP_OK);
    nimble_test_drain();
    nimble_test_value(BLE_HS_ETIMEOUT,11,NULL,0);
    solar_os_ble_hid_get_keepalive_status(&keepalive_status);
    assert(keepalive_status.attempts==3 && keepalive_status.last_status==ESP_FAIL);
    assert(device.connected && !hid.closing);
    timeout(NULL); assert(device.connected && !hid.closing); /* Queued before ready. */
    hid_event_t e; assert(xQueueReceive(events,&e,0)==pdTRUE && e.type==SOLAR_OS_BLE_HID_OPEN);
    assert(xQueueReceive(events,&e,0)==pdTRUE && e.type==SOLAR_OS_BLE_HID_BATTERY);
    assert(e.event.battery.level==87);
    uint8_t updated_battery=86;
    struct os_mbuf battery_mb={.len=1,.data=&updated_battery};
    struct ble_gap_event battery_notify={.type=BLE_GAP_EVENT_NOTIFY_RX,
        .notify_rx={.conn_handle=7,.attr_handle=83,.om=&battery_mb}};
    fake.gap(&battery_notify,fake.gap_arg);
    assert(xQueueReceive(events,&e,0)==pdTRUE && e.type==SOLAR_OS_BLE_HID_BATTERY);
    assert(e.event.battery.level==86);
    uint8_t report[]={0,0,4,0,0,0,0,0};
    struct os_mbuf mb={.len=8,.data=report};
    struct ble_gap_event notify={.type=BLE_GAP_EVENT_NOTIFY_RX,.notify_rx={.conn_handle=7,.attr_handle=5,.om=&mb}};
    fake.gap(&notify,fake.gap_arg);
    memset(report,0,sizeof(report));
    assert(xQueueReceive(events,&e,0)==pdTRUE && e.event.input.data[2]==4);
    assert(e.event.input.report_id==1);
    /* Queue pressure cannot drop CLOSE or leave pressed keys latched. */
    for(size_t i=0;i<HID_EVENT_MAX;++i)fake.gap(&notify,fake.gap_arg);
    assert(hid.closing && fake.terminate_calls==1);
    nimble_test_disconnect(); drain_policy(); assert(solar_os_ble_hid_idle());

    discover(); void *stale=token();
    timeout(NULL); assert(!hid.closing); /* An older phase's queued timer. */
    deadline_tick=xTaskGetTickCount();
    timeout(NULL); assert(fake.terminate_calls==1);
    nimble_test_disconnect(); nimble_test_drain();
    discover(); size_t n=hid.service_count;
    services_callback(7,&ok,&service,stale); assert(hid.service_count==n);
    hid.phase=READ_MAP; hid.map_len=HID_MAP_MAX;
    uint8_t byte=0; struct os_mbuf over={.len=1,.data=&byte}; struct ble_gatt_attr attr={.om=&over};
    assert(value_callback(7,&ok,&attr,token())==BLE_HS_ENOMEM);
    assert(hid.closing); nimble_test_disconnect(); nimble_test_drain();
    discover(); fake.submit_error=BLE_HS_ENOMEM;
    fake.svc(7,&ok,&service,fake.arg); fake.svc(7,&done,NULL,fake.arg);
    assert(hid.closing); nimble_test_disconnect(); nimble_test_drain();
    discover(); close_peer();
    solar_os_ble_hid_suspend();
    const uint8_t address[6]={1,2,3,4,5,6};
    assert(solar_os_ble_hid_open(address,0)==NULL);
    assert(solar_os_ble_hid_deinit()==ESP_OK);
    bool ids[256];
    assert(solar_os_ble_hid_report_map(map,sizeof(map),ids) && ids[1] && !ids[0]);
    for (size_t i=1;i<sizeof(map);++i) {
        /* Every truncation is either structurally valid or rejected; no reads past i. */
        uint8_t *cut=malloc(i); memcpy(cut,map,i);
        (void)solar_os_ble_hid_report_map(cut,i,ids); free(cut);
    }
    uint8_t invalid[]={0x05};
    assert(!solar_os_ble_hid_report_map(invalid,sizeof(invalid),ids));
    uint8_t underflow[]={0xc0};
    assert(!solar_os_ble_hid_report_map(underflow,sizeof(underflow),ids));
    uint8_t nesting[34]; for(size_t i=0;i<17;++i){nesting[i*2]=0xa1;nesting[i*2+1]=1;}
    assert(!solar_os_ble_hid_report_map(nesting,sizeof(nesting),ids));
    puts("HID client: failure, cancellation, bounds, subscription, input lifetime and stale callback tests passed");
}
