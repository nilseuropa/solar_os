#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "nimble_test_support.h"
#include "solar_os_ble_hid_report_map.h"
static int allocation, fail_at;
static uint32_t local_service, local_char;
esp_err_t solar_os_ble_keyboard_client_acquire(uint32_t timeout_ms)
{ (void)timeout_ms; return ESP_OK; }
void solar_os_ble_keyboard_client_release(void) {}
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
static void terminate_subscription(uint16_t conn, uint16_t handle)
{
    struct ble_gap_event e = {.type=BLE_GAP_EVENT_SUBSCRIBE,
        .subscribe={.conn_handle=conn,.attr_handle=handle,
                    .reason=BLE_GAP_SUBSCRIBE_REASON_TERM}};
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
static void name_start(server_peer_t *p)
{
    p->name_requested = p->name_complete = false;
    p->name_length = p->name_handle = 0;
    memset(p->name, 0, sizeof(p->name));
    const int before = fake.read_uuid_calls;
    lock(); server_host_name_request(p); server_host_name_request(p); unlock();
    assert(fake.read_uuid_calls == before+1 && fake.read_uuid == 0x2a00);
    assert(fake.last_start == 1 && fake.last_end == UINT16_MAX);
}
static void name_handle(server_peer_t *p)
{
    ble_gatt_attr_fn *lookup = fake.attr;
    void *arg = fake.arg;
    struct ble_gatt_error ok = {0};
    struct ble_gatt_attr attr = {.handle=17};
    assert(lookup(p->conn, &ok, &attr, arg) == 1);
    assert(fake.attr != lookup && fake.last_handle == 17);
    /* UUID-search completion must not complete the separate long read. */
    struct ble_gatt_error done = {.status=BLE_HS_EDONE};
    assert(lookup(p->conn, &done, NULL, arg) == 1 && !p->name_complete);
}
static int name_chunk(server_peer_t *p, size_t offset, const char *text, size_t length)
{
    struct os_mbuf buffer = {.len=length,.data=(uint8_t *)text};
    struct ble_gatt_attr attr = {.handle=17,.offset=offset,.om=&buffer};
    struct ble_gatt_error ok = {0};
    return fake.attr(p->conn, &ok, &attr, fake.arg);
}
static void name_done(server_peer_t *p, int status)
{
    struct ble_gatt_error error = {.status=status};
    assert(fake.attr(p->conn, &error, NULL, fake.arg) == 1);
}
static void test_names(server_peer_t *p)
{
    /* Short UTF-8 name, segmented long name, complete UTF-8 truncation,
     * unavailable/denied names, malformed encoding and immediate failure. */
    name_handle(p);
    assert(name_chunk(p, 0, "T\xc3\xa9l\xc3\xa9phone", 11) == 0);
    name_done(p, BLE_HS_EDONE);
    assert(p->name_complete && !strcmp(p->name, "T\xc3\xa9l\xc3\xa9phone"));
    name_start(p); name_handle(p);
    assert(name_chunk(p, 0, "Winter", 6) == 0);
    assert(name_chunk(p, 6, "mute", 4) == 0);
    name_done(p, BLE_HS_EDONE);
    assert(!strcmp(p->name, "Wintermute"));
    name_start(p); name_handle(p);
    char long_name[70]; memset(long_name, 'x', sizeof(long_name));
    long_name[62]=(char)0xc3; long_name[63]=(char)0xa9;
    assert(name_chunk(p, 0, long_name, sizeof(long_name)) == 1);
    assert(p->name_complete && strlen(p->name) == 62);
    name_start(p); name_done(p, BLE_HS_ENOENT);
    assert(p->name_complete && !p->name[0] && p->encrypted && p->bonded && !p->retiring);
    name_start(p); name_handle(p); name_done(p, BLE_HS_EAPP);
    assert(!p->name[0] && !p->retiring);
    name_start(p); name_handle(p);
    assert(name_chunk(p, 0, "\xc0\x80", 2) == 0); name_done(p, BLE_HS_EDONE);
    assert(!p->name[0]);
    name_start(p); name_handle(p);
    assert(name_chunk(p, 2, "bad offset", 10) == 1 && !p->name[0]);
    fake.submit_error = BLE_HS_EBUSY;
    name_start(p);
    assert(p->name_complete && !p->name[0] && !p->retiring);
    fake.submit_error = 0;
    /* Leave one unfinished read for teardown/reused-handle checks. */
    name_start(p); name_handle(p);
}
static void test_hid_boot_database(void)
{
    /* Startup allocation failures must leave no native service or lease. */
    for (int n=1;;n++) {
        nimble_test_reset(); allocation=0; fail_at=n;
        esp_err_t result=solar_os_ble_nimble_hid_prepare();
        fail_at=0;
        if (result==ESP_OK) break;
        assert(result==ESP_ERR_NO_MEM && (!peripheral || !peripheral->registered));
        assert(fake.server_add_calls==0 && fake.server_static_add_calls==0);
        solar_os_ble_backend_reset();
        assert(!peripheral && solar_os_ble_nimble_client_idle());
    }
    assert(peripheral->registered && peripheral->dormant);
    assert(solar_os_ble_nimble_client_idle());
    assert(fake.server_static_add_calls==1 && fake.server_add_calls==0 && fake.adv_calls==0);
    const uint16_t handle=hid_characteristic(SERVER_HID_KEYBOARD_INPUT)->handle;
    solar_os_ble_hid_request_t hid={.op=SOLAR_OS_BLE_HID_OP_START,.name="Boot HID",.manual=true};
    assert(execute_hid(42,&hid)==ESP_OK && !peripheral->dormant);
    assert(hid_characteristic(SERVER_HID_KEYBOARD_INPUT)->handle==handle);
    assert(fake.server_add_calls==0 && fake.adv_calls==0);
    solar_os_ble_backend_server_cancel(42); nimble_test_drain();
    assert(peripheral->dormant && peripheral->registered);
    assert(execute_hid(43,&hid)==ESP_OK);
    assert(fake.server_add_calls==0 && hid_characteristic(SERVER_HID_KEYBOARD_INPUT)->handle==handle);
    solar_os_ble_backend_server_cancel(43); nimble_test_drain();
    create_server(); /* Generic apps can replace the dormant service. */
    assert(fake.server_delete_calls==1 && peripheral->kind==SERVER_KIND_GENERIC);
    solar_os_ble_backend_reset();
    nimble_test_reset(); fake.server_count_error=BLE_HS_ENOMEM;
    assert(solar_os_ble_nimble_hid_prepare()==ESP_ERR_NO_MEM);
    assert(fake.server_static_add_calls==0 && fake.adv_calls==0);
    solar_os_ble_backend_reset();
    nimble_test_reset(); fake.server_add_error=BLE_HS_ENOMEM;
    assert(solar_os_ble_nimble_hid_prepare()==ESP_ERR_NO_MEM);
    assert(fake.adv_calls==0);
    solar_os_ble_backend_reset(); nimble_test_reset();
}
int main(void)
{
    nimble_test_reset(); assert(solar_os_ble_backend_register()==ESP_OK);
    test_hid_boot_database();
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
    om.next=NULL; om.len=177; assert(access_value(21,&ctx)==BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN);
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
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_PAIR};
    assert(execute_hid(77,&hid)==ESP_OK && peripheral->pairing);
    const int before_pair_delete=fake.store_delete_calls;
    connect_peer(31);
    assert(fake.store_delete_calls==before_pair_delete+1);
    assert(fake.security_calls==1 && peripheral->peer_count==1);
    assert(fake.security_mitm==1 && fake.security_sc==1); /* Fresh pairing defaults. */
    assert(!peripheral->advertising); /* HID accepts one active host. */
    /* Full bond storage must make room before NimBLE can emit a passkey.
     * Preserve even a disconnected input keyboard, other live peers, and
     * the host currently pairing. Local-only partial bonds are eligible. */
    struct ble_store_status_event full={.event_code=BLE_STORE_EVENT_FULL,
        .full={.obj_type=BLE_STORE_OBJ_TYPE_OUR_SEC,.conn_handle=31}};
    const ble_addr_t keyboard_bond={.val={1}}, live_bond={.val={2}},
        host_bond={.val={3}}, unused_bond={.val={4}};
    fake.keyboard_bond=keyboard_bond; fake.keyboard_bond_valid=true;
    fake.active_bonds[0]=live_bond; fake.active_bonds[1]=host_bond;
    fake.active_bond_count=2;
    fake.store_bonds[0][0]=keyboard_bond; fake.store_bonds[0][1]=live_bond;
    fake.store_bonds[0][2]=host_bond; fake.store_bonds[0][3]=unused_bond;
    fake.store_bond_count[0]=4;
    int deletes=fake.store_delete_calls;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==0);
    assert(fake.store_delete_calls==deletes+1 && fake.store_bond_count[0]==3);
    assert(!memcmp(&fake.deleted_bond,&unused_bond,sizeof(unused_bond)));
    /* No eligible bond is a reported error, never an input disconnection. */
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_ENOMEM);
    assert(fake.store_delete_calls==deletes+1);
    fake.store_bonds[1][0]=unused_bond; fake.store_bond_count[1]=1;
    full.full.obj_type=BLE_STORE_OBJ_TYPE_PEER_SEC;
    peripheral->pairing=false;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_ENOMEM);
    peripheral->pairing=true;
    full.full.conn_handle=99;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_ENOMEM);
    full.full.conn_handle=31;
    full.event_code=BLE_STORE_EVENT_OVERFLOW;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_ENOMEM);
    full.event_code=BLE_STORE_EVENT_FULL;
    fake.store_iterate_error=BLE_HS_EAPP;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_EAPP);
    fake.store_iterate_error=0; fake.store_delete_error=BLE_HS_EAPP;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==BLE_HS_EAPP);
    assert(fake.store_bond_count[1]==1);
    fake.store_delete_error=0;
    assert(solar_os_ble_nimble_store_status(&full,NULL)==0);
    assert(fake.store_bond_count[1]==0);
    const uint32_t hid_peer = peripheral->peers->id;
    struct ble_gap_event passkey_event={.type=BLE_GAP_EVENT_PASSKEY_ACTION,
        .passkey={.conn_handle=31}};
    fake.server_gap(&passkey_event,fake.server_gap_arg);
    assert(passkey_security_calls==1 && shared_security_calls==0);
    struct ble_gap_event repeat_pairing={.type=BLE_GAP_EVENT_REPEAT_PAIRING,
        .repeat_pairing={.conn_handle=31}};
    assert(fake.server_gap(&repeat_pairing,fake.server_gap_arg)==BLE_GAP_REPEAT_PAIRING_RETRY);
    assert(fake.store_delete_calls==before_pair_delete+5 && shared_security_calls==0);
    bool saw_passkey=false;
    int storage_failures=0;
    while (true) {
        hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_POLL};
        if (execute_hid(77,&hid)==ESP_ERR_NOT_FOUND) break;
        if (hid.event.type==SOLAR_OS_BLE_HID_PASSKEY) {
            assert(hid.event.peer==hid_peer && hid.event.passkey==12345);
            saw_passkey=true;
        }
        if (hid.event.type==SOLAR_OS_BLE_HID_SECURED && hid.event.status) {
            assert(hid.event.peer==hid_peer);
            assert(hid.event.status==BLE_HS_ENOMEM || hid.event.status==BLE_HS_EAPP);
            storage_failures++;
        }
    }
    assert(saw_passkey);
    assert(storage_failures==3);
    fake.encrypted=true; fake.bonded=true;
    fake.store_cccd_handle=keyboard->handle;
    fake.store_cccd_flags=1;
    struct ble_gap_event secured={.type=BLE_GAP_EVENT_ENC_CHANGE,
        .enc_change={.conn_handle=31}};
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    assert(!peripheral->pairing);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_STATUS};
    assert(execute_hid(77,&hid)==ESP_OK && hid.info.keyboard_subscribed);
    assert(!hid.info.mouse_subscribed && !hid.info.gamepad_subscribed);
    assert(fake.store_cccd_read_calls==3); /* Bonded reconnect restores input CCCDs. */
    assert(fake.store_cccd_write_calls==3); /* Mirror them explicitly for later leases. */
    subscribe_handle(31,keyboard->handle);
    subscribe_handle(31,mouse->handle);
    subscribe_handle(31,gamepad->handle);
    assert(fake.store_cccd_write_calls==6);
    assert(fake.store_cccd_written.chr_val_handle==gamepad->handle &&
        fake.store_cccd_written.flags==1);
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
    /* Security teardown is followed by GAP disconnect. It must not enqueue
     * a fake authentication failure or issue another terminate command. */
    const int before_broken_security=fake.terminate_calls;
    secured.enc_change.status=BLE_HS_ENOTCONN;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    assert(fake.terminate_calls==before_broken_security && !peripheral->count);
    assert(peripheral->peers->encrypted && !peripheral->peers->retiring);
    secured.enc_change.status=0;
    const uint16_t retained_keyboard_handle=keyboard->handle;
    const int hid_add_calls=fake.server_add_calls;
    const int before_close=fake.notify_calls;
    solar_os_ble_backend_server_cancel(77); nimble_test_drain();
    assert(peripheral && peripheral->closing && fake.notify_calls==before_close+3);
    const int writes_before_term=fake.store_cccd_write_calls;
    terminate_subscription(31,keyboard->handle);
    terminate_subscription(31,mouse->handle);
    terminate_subscription(31,gamepad->handle);
    assert(fake.store_cccd_write_calls==writes_before_term);
    assert(peripheral->hid_subscriptions[0]==1 &&
        peripheral->hid_subscriptions[1]==1 &&
        peripheral->hid_subscriptions[2]==1);
    disconnect_peer(31);
    assert(peripheral && peripheral->dormant && peripheral->registered);
    assert(peripheral->owner==SOLAR_OS_BLE_SESSION_INVALID && server_idle_locked());

    /* A new HID lease reuses the registered service and its ATT handles, so
     * bonded hosts can restore their handle-keyed CCCDs without re-pairing. */
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_START,.name="Reused HID"};
    assert(execute_hid(78,&hid)==ESP_OK && !peripheral->dormant);
    keyboard=hid_characteristic(SERVER_HID_KEYBOARD_INPUT);
    assert(keyboard->handle==retained_keyboard_handle);
    assert(fake.server_add_calls==hid_add_calls && peripheral->advertising);
    connect_peer(32);
    /* Exercise the retained in-memory fallback independently of the NimBLE
     * store. A completed subscription write from the first lease seeded it. */
    fake.store_cccd_handle=0;
    secured.enc_change.conn_handle=32;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_STATUS};
    assert(execute_hid(78,&hid)==ESP_OK && hid.info.keyboard_subscribed &&
        hid.info.mouse_subscribed && hid.info.gamepad_subscribed);
    solar_os_ble_backend_server_cancel(78); nimble_test_drain();
    disconnect_peer(32);
    assert(peripheral && peripheral->dormant && server_idle_locked());

    /* Reset drops the disconnected remembered host immediately, before a
     * replacement host connects. Preserve the input keyboard's security. */
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_START,.name="Reset HID"};
    assert(execute_hid(79,&hid)==ESP_OK);
    assert(peripheral->hid_bond_valid);
    const ble_addr_t previous_host=peripheral->hid_bond_addr;
    fake.store_bonds[0][0]=keyboard_bond;
    fake.store_bonds[0][1]=previous_host;
    fake.store_bond_count[0]=2;
    hid.op=SOLAR_OS_BLE_HID_OP_PAIR;
    fake.store_delete_error=BLE_HS_EAPP;
    assert(execute_hid(79,&hid)!=ESP_OK);
    assert(peripheral->hid_bond_valid && !peripheral->pairing);
    fake.store_delete_error=0;
    assert(execute_hid(79,&hid)==ESP_OK);
    assert(peripheral->pairing && !peripheral->hid_bond_valid);
    assert(!memcmp(&fake.deleted_bond,&previous_host,sizeof(previous_host)));
    assert(fake.store_bond_count[0]==1 &&
        !memcmp(&fake.store_bonds[0][0],&keyboard_bond,sizeof(keyboard_bond)));
    /* Fresh-boot state has no cached identity; recover it from persisted HID
     * subscriptions, with the same input-bond protection and error handling. */
    fake.store_cccd_written=(struct ble_store_value_cccd){
        .peer_addr=previous_host,.chr_val_handle=keyboard->handle,.flags=1};
    fake.store_bonds[0][1]=previous_host; fake.store_bond_count[0]=2;
    assert(execute_hid(79,&hid)==ESP_OK);
    assert(fake.store_bond_count[0]==1);
    fake.store_cccd_written.peer_addr=keyboard_bond;
    deletes=fake.store_delete_calls;
    assert(execute_hid(79,&hid)==ESP_OK);
    assert(fake.store_delete_calls==deletes && fake.store_bond_count[0]==1);
    fake.store_iterate_error=BLE_HS_EAPP;
    assert(execute_hid(79,&hid)!=ESP_OK);
    fake.store_iterate_error=0;
    solar_os_ble_backend_server_cancel(79); nimble_test_drain();
    assert(peripheral->dormant && server_idle_locked());

    /* Managed leases start idle even with a remembered host. Import legacy
     * public hosts from HID CCCDs, never the local input keyboard's bond. */
    fake.store_bonds[0][1]=previous_host; fake.store_bond_count[0]=2;
    fake.store_cccd_written=(struct ble_store_value_cccd){
        .peer_addr=previous_host,.chr_val_handle=keyboard->handle,.flags=1};
    const int adv_before_managed=fake.adv_calls;
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_START,.name="Managed HID",.manual=true};
    assert(execute_hid(80,&hid)==ESP_OK);
    assert(peripheral->manual && !peripheral->wants_advertising && !peripheral->advertising);
    assert(fake.adv_calls==adv_before_managed);
    hid.op=SOLAR_OS_BLE_HID_OP_HOSTS;
    assert(execute_hid(80,&hid)==ESP_OK && hid.host_count==1);
    assert(hid.hosts[0].bda[0]==previous_host.val[5] && hid.hosts[0].bda[5]==previous_host.val[0]);
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_CONNECT,.addr_type=previous_host.type};
    for (size_t i=0;i<6;i++) hid.bda[i]=previous_host.val[5-i];
    assert(execute_hid(80,&hid)==ESP_OK);
    /* Failure to configure the selected host filter must not expose an
     * unfiltered reconnect offer. A later poll may retry a busy controller. */
    fake.whitelist_error=BLE_HS_EBUSY;
    assert(server_advertise()==BLE_HS_EBUSY && !peripheral->advertising);
    assert(fake.adv_calls==adv_before_managed);
    fake.whitelist_error=0;
    assert(server_advertise()==0);
    assert(fake.adv_own==BLE_OWN_ADDR_PUBLIC && fake.adv_mode==BLE_GAP_CONN_MODE_UND);
    assert(fake.adv_filter==BLE_HCI_ADV_FILT_CONN && (fake.adv_flags & BLE_HS_ADV_F_DISC_GEN));
    assert(!memcmp(&fake.whitelist_peer,&previous_host,sizeof(previous_host)));
    assert(!memcmp(&fake.adv_target,&previous_host,sizeof(previous_host)));
    /* A different host is retired without negotiating security. */
    fake.address=keyboard_bond;
    int security_before=fake.security_calls;
    connect_peer(40);
    assert(fake.security_calls==security_before && peripheral->peers->retiring);
    disconnect_peer(40);
    assert(peripheral->advertising);
    hid.op=SOLAR_OS_BLE_HID_OP_DISCONNECT;
    assert(execute_hid(80,&hid)==ESP_OK && !peripheral->advertising && !peripheral->wants_advertising);
    /* Fresh pairing changes only our peripheral identity, retains BOTH bonds,
     * and never deletes the input keyboard or previous host. */
    deletes=fake.store_delete_calls;
    hid.op=SOLAR_OS_BLE_HID_OP_PAIR;
    assert(execute_hid(80,&hid)==ESP_OK);
    fake.initiating=true;
    assert(server_advertise()==BLE_HS_EBUSY && !peripheral->advertising);
    fake.initiating=false; fake.scan_active=true;
    solar_os_ble_hid_request_t retry_status={.op=SOLAR_OS_BLE_HID_OP_STATUS};
    assert(solar_os_ble_backend_hid_request(80,&retry_status)==ESP_OK);
    nimble_test_drain();
    assert(peripheral->advertising && fake.scan_cancel_calls==1);
    assert(fake.adv_own==BLE_OWN_ADDR_RANDOM && fake.adv_mode==BLE_GAP_CONN_MODE_UND);
    assert(fake.adv_filter==0 && (fake.adv_flags & BLE_HS_ADV_F_DISC_GEN));
    assert((fake.random_address[5]&0xc0)==0xc0 && fake.store_delete_calls==deletes);
    assert(fake.store_bond_count[0]==2);
    const ble_addr_t first_identity=peripheral->local_identity;
    assert(execute_hid(80,&hid)==ESP_OK && server_advertise()==0);
    assert(!server_same_address(&first_identity,&peripheral->local_identity));
    const ble_addr_t new_identity=peripheral->local_identity;
    const ble_addr_t new_host={.type=1,.val={10,11,12,13,14,0xcf}};
    fake.address=new_host;
    connect_peer(41);
    fake.store_bonds[0][2]=new_host; fake.store_bond_count[0]=3;
    fake.encrypted=fake.bonded=true;
    secured.enc_change.conn_handle=41;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    assert(peripheral->selected_host && !peripheral->pairing);
    assert(peripheral->peers->name_requested && !peripheral->peers->name_complete);
    test_names(peripheral->peers);
    ble_gatt_attr_fn *late_name = fake.attr;
    void *late_name_arg = fake.arg;
    hid.op=SOLAR_OS_BLE_HID_OP_HOSTS;
    assert(execute_hid(80,&hid)==ESP_OK && hid.host_count==2);
    server_host_store_t saved;
    assert(server_hosts_load(&saved)==ESP_OK && saved.count==2);
    assert(server_same_address(&saved.records[1].local,&new_identity));
    solar_os_ble_backend_server_cancel(80); nimble_test_drain(); disconnect_peer(41);
    /* Reopen and restore the stored local identity. Advertising is filtered
     * to the chosen host and other saved hosts cannot steal this connection. */
    hid=(solar_os_ble_hid_request_t){.op=SOLAR_OS_BLE_HID_OP_START,.name="Managed HID",.manual=true};
    assert(execute_hid(81,&hid)==ESP_OK && !peripheral->advertising);
    hid.op=SOLAR_OS_BLE_HID_OP_CONNECT; hid.addr_type=new_host.type;
    for(size_t i=0;i<6;i++) hid.bda[i]=new_host.val[5-i];
    assert(execute_hid(81,&hid)==ESP_OK && server_advertise()==0);
    assert(fake.adv_own==BLE_OWN_ADDR_RANDOM && fake.adv_mode==BLE_GAP_CONN_MODE_UND);
    assert(fake.adv_filter==BLE_HCI_ADV_FILT_CONN && (fake.adv_flags & BLE_HS_ADV_F_DISC_GEN));
    assert(!memcmp(&fake.whitelist_peer,&new_host,sizeof(new_host)));
    assert(!memcmp(fake.random_address,new_identity.val,6));
    connect_peer(42); secured.enc_change.conn_handle=42;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    /* A callback from the old lease cannot write into the replacement peer. */
    struct ble_gatt_error name_error={.status=BLE_HS_EDONE};
    assert(late_name(42,&name_error,NULL,late_name_arg)==1);
    assert(!peripheral->peers->name_complete);
    name_handle(peripheral->peers);
    subscribe_handle(42,keyboard->handle);
    solar_os_ble_hid_request_t report={.op=SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS,.key_count=1,.keys={4}};
    assert(execute_hid(81,&report)==ESP_OK); /* Reports do not wait for lookup. */
    assert(name_chunk(peripheral->peers,0,"Desktop",7)==0);
    name_done(peripheral->peers,BLE_HS_EDONE);
    hid.op=SOLAR_OS_BLE_HID_OP_HOSTS;
    assert(execute_hid(81,&hid)==ESP_OK && hid.host_count==2);
    assert(!hid.hosts[0].name[0] && !strcmp(hid.hosts[1].name,"Desktop"));
    server_host_store_t names_not_persisted;
    assert(server_hosts_load(&names_not_persisted)==ESP_OK);
    assert(!memcmp(&saved,&names_not_persisted,sizeof(saved))); /* No name in NVS. */
    hid.op=SOLAR_OS_BLE_HID_OP_STATUS;
    assert(execute_hid(81,&hid)==ESP_OK && hid.info.manual && hid.info.host_selected && !hid.info.pairing);
    assert(!strcmp(hid.info.host_name,"Desktop"));
    /* Repeated disconnect/select cycles retain the paired local identity and
     * reapply the chosen peer filter without reopening fresh pairing. */
    for (unsigned cycle=0; cycle<4; cycle++) {
        solar_os_ble_hid_request_t reconnect={.op=SOLAR_OS_BLE_HID_OP_DISCONNECT};
        assert(execute_hid(81,&reconnect)==ESP_OK && peripheral->peers->retiring);
        disconnect_peer(42);
        assert(!peripheral->advertising);
        reconnect.op=SOLAR_OS_BLE_HID_OP_CONNECT;
        reconnect.addr_type=new_host.type;
        for(size_t i=0;i<6;i++) reconnect.bda[i]=new_host.val[5-i];
        assert(execute_hid(81,&reconnect)==ESP_OK && server_advertise()==0);
        assert(!peripheral->pairing && fake.adv_own==BLE_OWN_ADDR_RANDOM);
        assert(fake.adv_mode==BLE_GAP_CONN_MODE_UND && fake.adv_filter==BLE_HCI_ADV_FILT_CONN);
        assert(!memcmp(fake.random_address,new_identity.val,6));
        assert(!memcmp(&fake.whitelist_peer,&new_host,sizeof(new_host)));
        /* Reconnect with unauthenticated/authenticated legacy and SC bonds.
         * The request must not upgrade an existing LTK or leak its policy
         * into input-keyboard pairing or a later fresh HID pairing. */
        fake.bond_authenticated=(cycle & 1) != 0;
        fake.bond_sc=(cycle & 2) != 0;
        security_before=fake.security_calls;
        connect_peer(42);
        assert(fake.security_calls==security_before+1);
        assert(fake.security_mitm==fake.bond_authenticated && fake.security_sc==fake.bond_sc);
        assert(ble_hs_cfg.sm_mitm==1 && ble_hs_cfg.sm_sc==1);
        fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
        subscribe_handle(42,keyboard->handle);
        reconnect.op=SOLAR_OS_BLE_HID_OP_STATUS;
        assert(execute_hid(81,&reconnect)==ESP_OK);
        assert(reconnect.info.connected && reconnect.info.encrypted && reconnect.info.bonded);
        assert(server_hid_ready(keyboard->id));
    }
    fake.submit_error=BLE_HS_EBUSY;
    assert(server_hid_security_initiate(42)==BLE_HS_EBUSY);
    assert(ble_hs_cfg.sm_mitm==1 && ble_hs_cfg.sm_sc==1);
    fake.submit_error=0;
    security_before=fake.security_calls;
    fake.bond_no_ltk=true;
    assert(server_hid_security_initiate(42)==BLE_HS_ENOENT);
    assert(fake.security_calls==security_before); /* Never pair instead of restoring a missing LTK. */
    fake.bond_no_ltk=false;
    /* A late security event from a retiring peer cannot save the new pairing
     * identity against the old host or complete a cancelled pairing flow. */
    peripheral->peers->retiring=true;
    peripheral->pairing=true;
    fake.nvs_error=ESP_ERR_NO_MEM;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    assert(peripheral->peers->bonded && peripheral->pairing);
    fake.nvs_error=0;
    peripheral->peers->retiring=false;
    peripheral->pairing=false;
    /* Deleting the connected host cancels advertising before retiring it. */
    hid.op=SOLAR_OS_BLE_HID_OP_FORGET;
    assert(execute_hid(81,&hid)==ESP_OK && peripheral->peers->retiring);
    assert(!peripheral->wants_advertising && !peripheral->selected_host);
    disconnect_peer(42);
    assert(!peripheral->advertising && fake.store_bond_count[0]==2);
    hid.op=SOLAR_OS_BLE_HID_OP_HOSTS;
    assert(execute_hid(81,&hid)==ESP_OK && hid.host_count==1);
    /* Input keyboard addresses cannot be used with host deletion. */
    hid.op=SOLAR_OS_BLE_HID_OP_FORGET; hid.addr_type=keyboard_bond.type;
    for(size_t i=0;i<6;i++) hid.bda[i]=keyboard_bond.val[5-i];
    deletes=fake.store_delete_calls;
    assert(execute_hid(81,&hid)==ESP_ERR_NOT_FOUND && fake.store_delete_calls==deletes);
    /* Failure to persist the new pairing must never expose a ready host. */
    hid.op=SOLAR_OS_BLE_HID_OP_PAIR;
    assert(execute_hid(81,&hid)==ESP_OK && server_advertise()==0);
    fake.address=new_host; connect_peer(43);
    fake.nvs_error=ESP_ERR_NO_MEM;
    secured.enc_change.conn_handle=43;
    fake.server_gap(&secured,fake.server_gap_arg); nimble_test_drain();
    assert(!peripheral->peers->bonded && !peripheral->peers->encrypted && peripheral->peers->retiring);
    assert(!peripheral->wants_advertising && !peripheral->pairing && !peripheral->selected_host);
    fake.nvs_error=0; disconnect_peer(43);
    /* Corrupt persisted host records fail closed. */
    fake.nvs_blob[0]=99;
    hid.op=SOLAR_OS_BLE_HID_OP_HOSTS;
    assert(execute_hid(81,&hid)==ESP_ERR_INVALID_SIZE);
    fake.nvs_blob[0]=1;
    solar_os_ble_backend_server_cancel(81); nimble_test_drain();

    /* The generic application server can replace a dormant native HID
     * service when a script requests the shared peripheral lease. */
    const int before_replace=fake.server_delete_calls;
    r=(solar_os_ble_server_request_t){.op=SOLAR_OS_BLE_SERVER_CREATE,
        .text="Generic after HID",.capacity=2};
    assert(execute(42,&r)==ESP_OK && peripheral->kind==SERVER_KIND_GENERIC);
    assert(fake.server_delete_calls==before_replace+1);
    solar_os_ble_backend_server_cancel(42); nimble_test_drain();
    assert(server_idle_locked());
    puts("BLE server: generic ownership plus encrypted keyboard, mouse and gamepad HOGP lifecycle OK");
    return 0;
}
