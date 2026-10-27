#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nimble_test_support.h"
#define SOLAR_OS_PACKAGE_SERVICE_BLE_STREAM 1
#include "../../src/services/solar_os_ble_nimble.c"

esp_err_t solar_os_ble_keyboard_client_acquire(uint32_t ms) { (void)ms; return ESP_OK; }
void solar_os_ble_keyboard_client_release(void) {}
void solar_os_ble_service_event(const solar_os_ble_backend_event_t *e) { (void)e; }
int solar_os_ble_nimble_security(struct ble_gap_event *e) { (void)e; return 0; }
int solar_os_ble_nimble_security_passkey(struct ble_gap_event *e, uint32_t *p)
{ (void)e; *p=123456; return 0; }

/* Model the SDK's ownership and one-credit receive contract. Tests drive host
 * events separately from application reads/writes, including cancellation. */
struct ble_l2cap_chan {
    ble_l2cap_event_fn *cb; void *arg;
    struct os_mbuf *rx, *stalled_tx;
    bool connected;
};
static struct ble_l2cap_chan channel;
static ble_l2cap_event_fn *listener;
static int send_status, connect_status, send_calls;
static int update_error, update_calls;
static uint16_t peer_mtu=246;
static uint8_t sent[32768]; static size_t sent_len;
int ble_gap_update_params(uint16_t conn, const struct ble_gap_upd_params *p)
{
    assert(conn==7 && p->itvl_min==p->itvl_max &&
        (p->itvl_min==12 || p->itvl_min==20) && p->latency==0 && p->max_ce_len==24);
    update_calls++;
    return p->itvl_min==12 ? update_error : 0;
}
int ble_gap_set_data_len(uint16_t conn, uint16_t octets, uint16_t us)
{ assert(conn==7 && octets==251 && us==2120); return 0; }
int ble_gap_set_prefered_le_phy(uint16_t conn, uint8_t tx, uint8_t rx, uint16_t opts)
{ assert(conn==7 && tx==2 && rx==2 && !opts); return 0; }
void os_mbuf_free_chain(struct os_mbuf *om)
{ while (om) { struct os_mbuf *next=om->next; free(om->data); free(om); om=next; } }
int ble_l2cap_create_server(uint16_t psm, uint16_t mtu, ble_l2cap_event_fn *cb, void *arg)
{ assert(psm==0x80 && mtu==COC_MTU && !arg); listener=cb; return 0; }
int ble_l2cap_connect(uint16_t conn, uint16_t psm, uint16_t mtu, struct os_mbuf *rx,
    ble_l2cap_event_fn *cb, void *arg)
{
    assert(conn==7 && psm==0x80 && mtu==COC_MTU && rx);
    channel=(struct ble_l2cap_chan){.cb=cb,.arg=arg,.rx=rx};
    if (connect_status) { os_mbuf_free_chain(rx); channel.rx=NULL; }
    return connect_status;
}
int ble_l2cap_recv_ready(struct ble_l2cap_chan *ch, struct os_mbuf *rx)
{ assert(!ch->rx && rx); ch->rx=rx; return 0; }
int ble_l2cap_get_chan_info(struct ble_l2cap_chan *ch, struct ble_l2cap_chan_info *info)
{ assert(ch==&channel); *info=(struct ble_l2cap_chan_info){.our_coc_mtu=COC_MTU,.peer_coc_mtu=peer_mtu}; return 0; }
int ble_l2cap_send(struct ble_l2cap_chan *ch, struct os_mbuf *tx)
{
    assert(ch==&channel && ch->connected && !ch->stalled_tx && tx->len<=peer_mtu);
    send_calls++;
    if (send_status==BLE_HS_EBUSY || send_status==BLE_HS_EBADDATA) return send_status;
    assert(sent_len+tx->len<=sizeof(sent));
    memcpy(sent+sent_len,tx->data,tx->len); sent_len+=tx->len;
    if (send_status==BLE_HS_ESTALLED) ch->stalled_tx=tx;
    else os_mbuf_free_chain(tx);
    return send_status;
}
int ble_l2cap_disconnect(struct ble_l2cap_chan *ch)
{
    struct ble_l2cap_event e={.type=BLE_L2CAP_EVENT_COC_DISCONNECTED,.disconnect={7,ch}};
    ch->connected=false;
    os_mbuf_free_chain(ch->rx); ch->rx=NULL;
    os_mbuf_free_chain(ch->stalled_tx); ch->stalled_tx=NULL;
    return ch->cb(&e,ch->arg);
}
static void connected(void)
{
    channel.connected=true;
    struct ble_l2cap_event e={.type=BLE_L2CAP_EVENT_COC_CONNECTED,.connect={0,7,&channel}};
    assert(!channel.cb(&e,channel.arg)); nimble_test_drain();
}
static void receive(const uint8_t *data, size_t len)
{
    assert(channel.rx); /* A peer needs an outstanding credit and buffer. */
    struct os_mbuf *rx=channel.rx; channel.rx=NULL;
    assert(!os_mbuf_append(rx,data,len));
    struct ble_l2cap_event e={.type=BLE_L2CAP_EVENT_COC_DATA_RECEIVED,.receive={7,&channel,rx}};
    assert(!channel.cb(&e,channel.arg)); nimble_test_drain();
}
static void unstalled(int status)
{
    os_mbuf_free_chain(channel.stalled_tx); channel.stalled_tx=NULL;
    struct ble_l2cap_event e={.type=BLE_L2CAP_EVENT_COC_TX_UNSTALLED,.tx_unstalled={7,&channel,status}};
    assert(!channel.cb(&e,channel.arg)); nimble_test_drain();
}
static uint32_t open_channel(void)
{
    uint32_t token=0;
    assert(!solar_os_ble_backend_coc_open(42,7,false,&token));
    nimble_test_drain(); return token;
}
int main(void)
{
    nimble_test_reset(); assert(!solar_os_ble_backend_register());
    clients=calloc(1,sizeof(*clients)); assert(clients); clients->conn=7;
    uint32_t token=open_channel(); size_t n;
    uint8_t input[8192], output[8192];
    for (size_t i=0;i<sizeof(input);i++) input[i]=(uint8_t)(i*17+i/256);
    assert(!solar_os_ble_backend_coc_write(token,input,100,&n) && n==100);
    connected();
    assert(sent_len==100 && !memcmp(sent,input,100)); sent_len=0; send_calls=0;
    /* Short SDUs consume a full credit; a paused reader eventually stops all
     * receive grants, without data loss, allocation growth, or busy looping. */
    size_t total=0;
    while (channel.rx) { size_t len=total%3 ? 113 : COC_MTU; receive(input+total,len); total+=len; }
    assert(total<=COC_BUFFER_SIZE && total>COC_BUFFER_SIZE-COC_MTU);
    assert(!solar_os_ble_backend_coc_read(token,output,37,&n) && n==37);
    nimble_test_drain();
    size_t rest=0;
    assert(!solar_os_ble_backend_coc_read(token,output+37,sizeof(output)-37,&rest));
    assert(n+rest==total && !memcmp(output,input,total));
    nimble_test_drain(); assert(channel.rx);
    /* Wraparound and arbitrary read boundaries preserve a byte stream. */
    for (int j=0;j<12;j++) {
        receive(input+j*11,211);
        assert(!solar_os_ble_backend_coc_read(token,output,100,&n) && n==100);
        assert(!solar_os_ble_backend_coc_read(token,output+100,111,&n) && n==111);
        assert(!memcmp(output,input+j*11,211)); nimble_test_drain();
    }
    /* Allocation pressure does not consume accepted TX or spin the host. */
    fake.mbuf_fail=true;
    assert(!solar_os_ble_backend_coc_write(token,input,sizeof(input),&n) && n==COC_BUFFER_SIZE);
    nimble_test_drain(); assert(!send_calls);
    fake.mbuf_fail=false; send_status=BLE_HS_ESTALLED;
    solar_os_ble_coc_info_t info;
    assert(!solar_os_ble_backend_coc_status(token,&info)); nimble_test_drain();
    assert(send_calls==1 && sent_len==COC_MTU && channel.stalled_tx);
    assert(!solar_os_ble_backend_coc_status(token,&info)); nimble_test_drain(); assert(send_calls==1);
    send_status=0; unstalled(0);
    assert(sent_len==COC_BUFFER_SIZE && !memcmp(sent,input,COC_BUFFER_SIZE));
    assert(!solar_os_ble_backend_coc_write(token,input+COC_BUFFER_SIZE,COC_BUFFER_SIZE,&n) && n==COC_BUFFER_SIZE);
    nimble_test_drain(); assert(sent_len==sizeof(input) && !memcmp(sent,input,sizeof(input)));
    /* A partial transmit failure ends the claim, never retries the SDU. */
    send_status=BLE_HS_ESTALLED;
    assert(!solar_os_ble_backend_coc_write(token,input,20,&n)); nimble_test_drain();
    unstalled(BLE_HS_ENOMEM);
    assert(solar_os_ble_backend_coc_read(token,output,1,&n)==ESP_FAIL);
    solar_os_ble_backend_coc_close(token); nimble_test_drain(); assert(!coc_find(token));
    /* Smaller remote MTUs, close while connecting, stale tokens, and immediate
     * connect failure all leave bounded storage and no cross-session bytes. */
    send_status=0; peer_mtu=73; sent_len=0;
    int updates_before=update_calls;
    update_error=BLE_HS_EBUSY;
    uint32_t next=open_channel(); connected();
    assert(update_calls==updates_before+2);
    update_error=0;
    /* A late callback from the retired token cannot fail a fresh channel even
     * when the host reuses the same channel address. */
    struct ble_l2cap_event stale={.type=BLE_L2CAP_EVENT_COC_TX_UNSTALLED,
        .tx_unstalled={7,&channel,BLE_HS_ENOMEM}};
    assert(!coc_event(&stale,(void *)(uintptr_t)token));
    assert(!solar_os_ble_backend_coc_status(next,&info) && info.connected && !info.failed);
    assert(next!=token && solar_os_ble_backend_coc_write(token,input,1,&n)==ESP_FAIL);
    assert(!solar_os_ble_backend_coc_write(next,input,300,&n)); nimble_test_drain();
    assert(sent_len==300 && !memcmp(sent,input,300));
    solar_os_ble_backend_coc_close(next); nimble_test_drain(); assert(!coc_find(next));
    for (size_t i=0;i<3;i++) {
        const int errors[]={BLE_HS_ENOMEM,BLE_HS_EBUSY,BLE_HS_EBADDATA};
        send_status=errors[i]; next=open_channel(); connected();
        assert(!solar_os_ble_backend_coc_write(next,input,50,&n)); nimble_test_drain();
        assert(solar_os_ble_backend_coc_read(next,output,1,&n)==ESP_FAIL);
        solar_os_ble_backend_coc_close(next); nimble_test_drain(); assert(!coc_find(next));
    }
    send_status=0;
    next=open_channel(); solar_os_ble_backend_coc_close(next); nimble_test_drain();
    assert(coc_find(next)); connected(); assert(!coc_find(next));
    connect_status=BLE_HS_ENOMEM;
    next=open_channel(); assert(!solar_os_ble_backend_coc_status(next,&info) && info.failed);
    solar_os_ble_backend_coc_close(next); nimble_test_drain(); assert(!coc_find(next));
    /* Listener accepts only its own peripheral peer, then refuses a second
     * channel until the claim is released, even after disconnect. */
    peripheral=calloc(1,sizeof(*peripheral)); peripheral->owner=42;
    peripheral->peers=calloc(1,sizeof(*peripheral->peers)); peripheral->peers->conn=7;
    assert(!solar_os_ble_backend_coc_open(42,0xffff,true,&next)); nimble_test_drain();
    channel=(struct ble_l2cap_chan){.cb=listener};
    struct ble_l2cap_event accept={.type=BLE_L2CAP_EVENT_COC_ACCEPT,.accept={8,73,&channel}};
    assert(listener(&accept,NULL)==BLE_HS_EBUSY);
    accept.accept.conn_handle=7; assert(!listener(&accept,NULL)); connected();
    assert(listener(&accept,NULL)==BLE_HS_EBUSY);
    receive(input,73); assert(!solar_os_ble_backend_coc_read(next,output,73,&n) && n==73);
    nimble_test_drain(); assert(!ble_l2cap_disconnect(&channel)); nimble_test_drain();
    assert(listener(&accept,NULL)==BLE_HS_EBUSY);
    solar_os_ble_backend_coc_close(next); nimble_test_drain(); assert(!coc_find(next));
    /* Host teardown (including sleep) invalidates a still-owned stream and
     * frees its copied bytes. Calls using its stale token enqueue no work on
     * a stopped host. */
    connect_status=0;
    next=open_channel(); connected(); receive(input,73);
    assert(!ble_l2cap_disconnect(&channel)); nimble_test_drain();
    solar_os_ble_backend_reset(); nimble_test_drain();
    assert(solar_os_ble_backend_coc_read(next,output,1,&n)==ESP_FAIL);
    solar_os_ble_nimble_host_stopped();
    for (size_t i=0;i<COC_CHANNELS;i++) assert(!coc_channels[i]);
    puts("BLE CoC tests passed");
}
