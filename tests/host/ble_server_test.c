#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nimble_test_support.h"
#include "solar_os_ble_hid_report_map.h"
static int allocation, fail_at;
static uint32_t local_service, local_char;
static void *server_test_calloc(size_t n, size_t size)
{ return ++allocation == fail_at ? NULL : calloc(n, size); }
#define calloc server_test_calloc
#include "../../src/services/solar_os_ble_nimble.c"
#undef calloc
void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event) { (void)event; }
static int shared_security_calls, passkey_security_calls;
int solar_os_ble_nimble_security(struct ble_gap_event *event)
{ (void)event; shared_security_calls++; return 0; }
int solar_os_ble_nimble_security_passkey(struct ble_gap_event *event, uint32_t *passkey)
{ (void)event; passkey_security_calls++; *passkey = 12345; return 0; }

static esp_err_t execute(uint32_t owner, solar_os_ble_server_request_t *r)
{
    lock(); esp_err_t result = server_execute(owner, r); unlock(); return result;
}
static esp_err_t execute_hid(uint32_t owner, solar_os_ble_hid_request_t *r)
{
    lock(); esp_err_t result = server_hid_execute(owner, r); unlock(); return result;
}
static server_char_t *hid_characteristic(uint8_t kind)
{
    for (server_service_t *s=peripheral->services;s;s=s->next)
        for (server_char_t *c=s->chars;c;c=c->next)
            if (c->hid_kind==kind) return c;
    return NULL;
}
static void connect_peer(uint16_t conn)
{
    struct ble_gap_event e = {.type=BLE_GAP_EVENT_CONNECT, .connect={.conn_handle=conn}};
    fake.server_gap(&e, fake.server_gap_arg); nimble_test_drain();
}
static void disconnect_peer(uint16_t conn)
{
    struct ble_gap_event e = {.type=BLE_GAP_EVENT_DISCONNECT, .disconnect={.reason=19,.conn={.conn_handle=conn}}};
    fake.server_gap(&e, fake.server_gap_arg); nimble_test_drain();
}
static void subscribe_peer(uint16_t conn, bool notify, bool indicate)
{
    struct ble_gap_event e = {.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.conn_handle=conn,.attr_handle=100,.cur_notify=notify,.cur_indicate=indicate}};
    fake.server_gap(&e,fake.server_gap_arg); nimble_test_drain();
}
static void subscribe_handle(uint16_t conn, uint16_t handle)
{
    struct ble_gap_event e = {.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.conn_handle=conn,.attr_handle=handle,.cur_notify=true}};
    fake.server_gap(&e,fake.server_gap_arg); nimble_test_drain();
}
static void create_server(void)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_CREATE,.text="SolarOS Test",.capacity=2};
    assert(execute(42,&r)==ESP_OK);
    r = (solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SERVICE,.text="1234"};
    assert(execute(42,&r)==ESP_OK); local_service=r.id;
    assert(execute(42,&r)==ESP_ERR_INVALID_ARG); /* Duplicate service UUID. */
    r = (solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC,.parent=local_service,
        .text="abcd",.properties=0x3e,.value_len=3,.value={0,0xff,0x80}};
    assert(execute(42,&r)==ESP_OK); local_char=r.id;
}
static void start_server(void)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_START};
    assert(execute(42,&r)==ESP_OK && peripheral->registered && peripheral->advertising);
    assert(!strcmp(fake.device_name,"SolarOS Test"));
}
static void drain_events(void)
{
    solar_os_ble_server_request_t r = {.op=SOLAR_OS_BLE_SERVER_POLL};
    while (execute(42,&r)==ESP_OK) {}
}
struct request_thread { solar_os_ble_server_request_t request; esp_err_t result; };
static void *request_worker(void *arg)
{
    struct request_thread *work=arg;
    work->result=solar_os_ble_backend_server_request(42,&work->request);
    return NULL;
}
static int access_value(uint16_t conn, struct ble_gatt_access_ctxt *ctx)
{
    const struct ble_gatt_chr_def *c = fake.server_definitions[0].characteristics;
    return c->access_cb(conn,*c->val_handle,ctx,c->arg);
}
int main(void)
{
    nimble_test_reset(); assert(solar_os_ble_backend_register()==ESP_OK);
    /* Every allocation while defining and starting the peripheral is recoverable. */
    for (int n=1;n<=7;n++) {
        allocation=0; fail_at=n;
        solar_os_ble_server_request_t r={.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Test",.capacity=2};
        esp_err_t result=execute(42,&r);
        if (!result) { r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SERVICE,.text="1234"}; result=execute(42,&r); }
        if (!result) { r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CHARACTERISTIC,.parent=r.id,.text="abcd",.properties=0x3e}; result=execute(42,&r); }
        if (!result) { r.op=SOLAR_OS_BLE_SERVER_START; result=execute(42,&r); }
        assert(result==ESP_ERR_NO_MEM);
        fail_at=0; solar_os_ble_backend_server_cancel(42); nimble_test_drain(); assert(!peripheral);
    }
    create_server();
    solar_os_ble_server_request_t r={.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(execute(43,&r)==ESP_ERR_INVALID_STATE);
    solar_os_ble_backend_server_cancel(43); nimble_test_drain(); assert(peripheral && !peripheral->closing);
    fake.server_add_error=BLE_HS_ENOMEM; r.op=SOLAR_OS_BLE_SERVER_START;
    assert(execute(42,&r)==ESP_ERR_NO_MEM && !peripheral->registered && !peripheral->advertising);
    fake.server_add_error=0; start_server();
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SERVICE,.text="4321"};
    assert(execute(42,&r)==ESP_ERR_INVALID_STATE); /* Published definitions are immutable. */
    connect_peer(21); connect_peer(22);
    assert(peripheral->peer_count==2 && server_used_locked()==3); /* Two links + advertising slot. */
    const uint8_t address[6]={1,2,3,4,5,6};
    assert(solar_os_ble_backend_connect(9,10,address,0)==SOLAR_OS_BLE_ERR_CAPACITY);
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_STOP};
    assert(execute(42,&r)==ESP_OK && server_used_locked()==2 && peripheral->peer_count==2);
    assert(solar_os_ble_backend_connect(9,10,address,0)==ESP_OK);
    assert(solar_os_ble_backend_cancel(9)==ESP_OK); nimble_test_drain(); assert(!clients);
    start_server();
    uint32_t first=server_peer(0,21)->id;
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_PEER};
    assert(execute(42,&r)==ESP_OK && r.event.peer==first && r.event.mtu==517);
    r.peer=r.event.peer; assert(execute(42,&r)==ESP_OK && r.event.peer>first);
    r.peer=r.event.peer; assert(execute(42,&r)==ESP_ERR_NOT_FOUND);

    uint8_t value[64]; for(size_t i=0;i<sizeof(value);i++) value[i]=(uint8_t)(i*7);
    struct os_mbuf tail={.data=value+20,.len=44}, om={.data=value,.len=20,.next=&tail};
    struct ble_gatt_access_ctxt ctx={.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&om};
    assert(access_value(99,&ctx)==BLE_ATT_ERR_UNLIKELY); /* A keyboard/client link cannot access app values. */
    assert(access_value(21,&ctx)==BLE_ATT_ERR_INSUFFICIENT_RES); /* Connect events filled the queue. */
    assert(server_characteristic(local_char,0)->len==3 && peripheral->dropped==1);
    drain_events(); assert(access_value(21,&ctx)==0);
    memset(value,0,sizeof(value)); /* Queued data and stored data are independent copies. */
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_POLL};
    assert(execute(42,&r)==ESP_OK && r.event.type==SOLAR_OS_BLE_SERVER_WRITE && r.event.value_len==64);
    assert(r.event.value[1]==7 && server_characteristic(local_char,0)->value[1]==7);
    ctx.offset=1; assert(access_value(21,&ctx)==BLE_ATT_ERR_INVALID_OFFSET); ctx.offset=0;
    om.next=NULL; om.len=129; assert(access_value(21,&ctx)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
    struct os_mbuf read={0}; ctx=(struct ble_gatt_access_ctxt){.op=BLE_GATT_ACCESS_OP_READ_CHR,.om=&read,.offset=10};
    assert(access_value(21,&ctx)==0 && read.len==64 && read.data[1]==7); free(read.data);
    read=(struct os_mbuf){0}; ctx.offset=65;
    assert(access_value(21,&ctx)==BLE_ATT_ERR_INVALID_OFFSET);

    drain_events(); subscribe_peer(21,true,true); drain_events();
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SEND,.id=local_char,.peer=first,.value_len=64};
    memcpy(r.value,server_characteristic(local_char,0)->value,64);
    assert(execute(42,&r)==ESP_OK && fake.written_len==64 && fake.written[1]==7);
    r.indicate=true; assert(execute(42,&r)==ESP_OK);
    assert(execute(42,&r)==ESP_ERR_INVALID_STATE); /* One in-flight indication per connection. */
    struct ble_gap_event tx={.type=BLE_GAP_EVENT_NOTIFY_TX,
        .notify_tx={.conn_handle=21,.attr_handle=100,.indication=true,.status=0}};
    fake.server_gap(&tx,fake.server_gap_arg); assert(server_peer(first,0)->indication_handle==100);
    tx.notify_tx.status=BLE_HS_EDONE; fake.server_gap(&tx,fake.server_gap_arg);
    assert(!server_peer(first,0)->indication_handle);
    solar_os_ble_server_request_t polled={.op=SOLAR_OS_BLE_SERVER_POLL};
    assert(execute(42,&polled)==ESP_OK && polled.event.indicate && !polled.event.status);
    fake.mtu=23; assert(execute(42,&r)==ESP_ERR_INVALID_SIZE); fake.mtu=517;
    fake.mbuf_fail=true; assert(execute(42,&r)==ESP_ERR_NO_MEM); fake.mbuf_fail=false;
    fake.submit_error=BLE_HS_ENOMEM; assert(execute(42,&r)==ESP_ERR_NO_MEM && !server_peer(first,0)->indication_handle);
    fake.submit_error=0; subscribe_peer(21,false,false); assert(execute(42,&r)==ESP_ERR_INVALID_STATE);
    drain_events();
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_SET,.id=local_char};
    assert(execute(42,&r)==ESP_OK && !server_characteristic(local_char,0)->len && !peripheral->count);

    /* Closing hides data immediately, but storage survives until disconnect callbacks drain. */
    void *old_epoch=fake.server_gap_arg;
    solar_os_ble_backend_server_cancel(42); nimble_test_drain();
    assert(peripheral && peripheral->closing && !peripheral->advertising && !server_idle_locked());
    assert(access_value(21,&ctx)==BLE_ATT_ERR_UNLIKELY);
    disconnect_peer(21); assert(peripheral); disconnect_peer(22); assert(!peripheral);
    create_server(); start_server();
    assert(execute(42,&r)==ESP_ERR_INVALID_ARG); /* Old characteristic ID cannot alias the new lease. */
    struct ble_gap_event stale={.type=BLE_GAP_EVENT_ADV_COMPLETE};
    fake.server_gap(&stale,old_epoch); assert(peripheral->advertising);
    solar_os_ble_backend_server_cancel(0); nimble_test_drain(); assert(server_idle_locked());

    /* Abandoned queued requests cannot create a server after timeout. */
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Timed out"};
    assert(solar_os_ble_backend_server_request(42,&r)==ESP_ERR_TIMEOUT);
    assert(server_command && server_command->abandoned);
    nimble_test_drain(); assert(!server_command && !peripheral);
    create_server(); start_server();
    fake.server_delete_error=BLE_HS_EBUSY;
    solar_os_ble_backend_server_cancel(42); nimble_test_drain(); assert(peripheral && peripheral->closing);
    fake.server_delete_error=0;
    solar_os_ble_backend_reset(); assert(server_idle_locked());
    struct request_thread work={.request={.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Threaded"}};
    pthread_t thread;
    assert(!pthread_create(&thread,NULL,request_worker,&work));
    nimble_test_wait_event(); nimble_test_drain(); assert(!pthread_join(thread,NULL));
    assert(work.result==ESP_OK && peripheral);
    work.request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(solar_os_ble_backend_server_request(42,&work.request)==ESP_OK && !server_command);
    solar_os_ble_backend_server_cancel(42); nimble_test_drain(); assert(!peripheral);
    work.request=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CREATE,.text="Cancelled"};
    assert(!pthread_create(&thread,NULL,request_worker,&work)); nimble_test_wait_event();
    solar_os_ble_backend_server_cancel(42); nimble_test_drain(); assert(!pthread_join(thread,NULL));
    assert(work.result==SOLAR_OS_BLE_ERR_CANCELLED && !peripheral);
    assert(!pthread_create(&thread,NULL,request_worker,&work)); nimble_test_wait_event();
    solar_os_ble_backend_reset(); nimble_test_drain(); assert(!pthread_join(thread,NULL));
    assert(work.result==SOLAR_OS_BLE_ERR_CANCELLED && server_idle_locked());

    /* Native composite HOGP owns the same peripheral lease and exposes only
     * typed reports. Descriptors and encryption stay below the script API. */
    solar_os_ble_hid_request_t hid={.op=SOLAR_OS_BLE_HID_OP_START,.name="SolarOS HID"};
    assert(execute_hid(77,&hid)==ESP_OK && peripheral->kind==SERVER_KIND_HID);
    assert(peripheral->registered && peripheral->advertising && fake.appearance==0x03c0);
    assert(!strcmp(fake.device_name,"SolarOS HID"));
    assert(ble_uuid_u16(&peripheral->services->uuid.u)==0x1812);
    server_char_t *map=hid_characteristic(SERVER_HID_REPORT_MAP);
    server_char_t *keyboard=hid_characteristic(SERVER_HID_KEYBOARD_INPUT);
    server_char_t *output=hid_characteristic(SERVER_HID_KEYBOARD_OUTPUT);
    server_char_t *mouse=hid_characteristic(SERVER_HID_MOUSE_INPUT);
    server_char_t *gamepad=hid_characteristic(SERVER_HID_GAMEPAD_INPUT);
    assert(map && map->static_value==server_hid_report_map && map->len>128);
    bool keyboard_ids[256]={0};
    assert(solar_os_ble_hid_report_map(map->static_value,map->len,keyboard_ids));
    assert(keyboard_ids[1] && !keyboard_ids[2] && !keyboard_ids[3]);
    assert(keyboard && mouse && gamepad && output && keyboard->descriptor_definitions);
    assert(ble_uuid_u16(keyboard->descriptor_definitions[0].uuid)==0x2908);
    assert(keyboard->flags & BLE_GATT_CHR_F_READ_ENC);
    assert(keyboard->flags & BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC);
    solar_os_ble_server_request_t generic={.op=SOLAR_OS_BLE_SERVER_STATUS};
    assert(execute(77,&generic)==ESP_ERR_INVALID_STATE);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS,
        .axis=0,.value=INT16_MAX};
    assert(execute_hid(77,&hid)==ESP_ERR_INVALID_STATE);
    connect_peer(31); assert(fake.security_calls==1 && peripheral->peer_count==1);
    assert(!peripheral->advertising); /* HID accepts one active host. */
    const uint32_t hid_peer = peripheral->peers->id;
    struct ble_gap_event passkey_event={.type=BLE_GAP_EVENT_PASSKEY_ACTION,
        .passkey={.conn_handle=31}};
    fake.server_gap(&passkey_event,fake.server_gap_arg);
    assert(passkey_security_calls==1 && shared_security_calls==0);
    struct ble_gap_event repeat_pairing={.type=BLE_GAP_EVENT_REPEAT_PAIRING,
        .repeat_pairing={.conn_handle=31}};
    assert(fake.server_gap(&repeat_pairing,fake.server_gap_arg)==BLE_GAP_REPEAT_PAIRING_RETRY);
    assert(fake.store_delete_calls==1 && shared_security_calls==0);
    bool saw_passkey=false;
    while (true) {
        hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_POLL};
        if (execute_hid(77,&hid)==ESP_ERR_NOT_FOUND) break;
        if (hid.event.type==SOLAR_OS_BLE_HID_PASSKEY) {
            assert(hid.event.peer==hid_peer && hid.event.passkey==12345);
            saw_passkey=true;
        }
    }
    assert(saw_passkey);
    fake.encrypted=true; fake.bonded=true;
    struct ble_gap_event secured={.type=BLE_GAP_EVENT_ENC_CHANGE,
        .enc_change={.conn_handle=31}};
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    subscribe_handle(31,keyboard->handle);
    subscribe_handle(31,mouse->handle);
    subscribe_handle(31,gamepad->handle);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_STATUS};
    assert(execute_hid(77,&hid)==ESP_OK && hid.info.connected && hid.info.encrypted);
    assert(hid.info.bonded && hid.info.keyboard_subscribed &&
        hid.info.mouse_subscribed && hid.info.gamepad_subscribed);
    while (true) {
        hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_POLL};
        if (execute_hid(77,&hid)==ESP_ERR_NOT_FOUND) break;
    }
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS,
        .key_count=2,.keys={SOLAR_OS_HID_KEY_LEFT_CTRL,0x04}};
    assert(execute_hid(77,&hid)==ESP_OK && fake.written_len==8);
    assert(fake.written[0]==1 && fake.written[2]==4);
    hid.op=SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE;
    assert(execute_hid(77,&hid)==ESP_OK && fake.written[0]==0 && fake.written[2]==0);
    const int before_mouse=fake.notify_calls;
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_MOUSE_MOVE,.x=200,.y=-200};
    assert(execute_hid(77,&hid)==ESP_OK && fake.notify_calls==before_mouse+2);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS,
        .axis=0,.value=INT16_MAX};
    assert(execute_hid(77,&hid)==ESP_OK);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_GAMEPAD_BUTTON,
        .button=1,.pressed=true};
    assert(execute_hid(77,&hid)==ESP_OK);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_GAMEPAD_HAT,.hat=2};
    assert(execute_hid(77,&hid)==ESP_OK);
    hid.op=SOLAR_OS_BLE_HID_OP_GAMEPAD_SEND;
    assert(execute_hid(77,&hid)==ESP_OK && fake.written_len==11);
    assert(fake.written[0]==127 && fake.written[6]==2 && fake.written[7]==1);
    uint8_t leds=SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK;
    struct os_mbuf led_value={.data=&leds,.len=1};
    ctx=(struct ble_gatt_access_ctxt){.op=BLE_GATT_ACCESS_OP_WRITE_CHR,.om=&led_value};
    assert(server_access(31,output->handle,&ctx,output)==0);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_POLL};
    assert(execute_hid(77,&hid)==ESP_OK &&
        hid.event.type==SOLAR_OS_BLE_HID_KEYBOARD_LEDS &&
        hid.event.keyboard_leds==SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK);
    peripheral->count=peripheral->capacity;
    leds=SOLAR_OS_BLE_HID_KEYBOARD_LED_NUM_LOCK;
    assert(server_access(31,output->handle,&ctx,output)==BLE_ATT_ERR_INSUFFICIENT_RES);
    assert(peripheral->keyboard_leds==SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK &&
        output->value[0]==SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK);
    peripheral->head=0; peripheral->count=0;
    const int before_close=fake.notify_calls;
    solar_os_ble_backend_server_cancel(77); nimble_test_drain();
    assert(peripheral && peripheral->closing && fake.notify_calls==before_close+3);
    disconnect_peer(31); assert(server_idle_locked());
    puts("BLE server: generic ownership plus encrypted keyboard, mouse and gamepad HOGP lifecycle OK");
    return 0;
}
