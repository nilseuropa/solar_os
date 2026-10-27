/* Included by the NimBLE backend, after the shared peripheral adapter. All
 * host calls run on its queue or in a host callback, under the recursive lock.
 * Bulk data never enters the lossy GATT application event queues. */
#include "host/ble_l2cap.h"
#include "solar_os_ble_stream.h"

#define COC_CHANNELS 2
#define COC_BUFFER_SIZE 4096
/* One credit per SDU, including its two-byte header. This matters for short
 * SDUs: with multiple initial credits the peer could start another SDU after
 * our reader fills the queue and before we supply its next receive buffer.
 * Do not raise this above MPS without implementing explicit credit accounting. */
/* Also fit the four-byte L2CAP and two-byte SDU headers in a 251-byte
 * link-layer packet. An extra byte otherwise creates a second radio packet. */
#define COC_MTU ((MYNEWT_VAL(BLE_L2CAP_COC_MPS) - 2) < 245 ? \
    (MYNEWT_VAL(BLE_L2CAP_COC_MPS) - 2) : 245)
_Static_assert(COC_MTU >= 23 && COC_MTU <= COC_BUFFER_SIZE, "CoC MTU exceeds buffer");

typedef struct {
    uint8_t data[COC_BUFFER_SIZE];
    size_t head, size;
} coc_fifo_t;
typedef struct {
    uint32_t token, owner;
    uint16_t conn, tx_mtu;
    bool listener, started, connecting, connected, failed, closing, rx_armed, stalled;
    bool refresh_info;
    uint32_t connection_interval_us;
    struct ble_l2cap_chan *chan;
    coc_fifo_t rx, tx;
} coc_channel_t;

/* Heap storage exists only while claimed or awaiting host retirement. */
static coc_channel_t *coc_channels[COC_CHANNELS];
static uint32_t coc_ticket;
static bool coc_server_registered;
static int coc_event(struct ble_l2cap_event *event, void *arg);

static void coc_queue(void)
{
    if (command_ready) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
}
static coc_channel_t *coc_find(uint32_t token)
{
    for (size_t i = 0; i < COC_CHANNELS; i++)
        if (coc_channels[i] && coc_channels[i]->token == token) return coc_channels[i];
    return NULL;
}
static void coc_free(coc_channel_t *c)
{
    for (size_t i = 0; i < COC_CHANNELS; i++)
        if (coc_channels[i] == c) coc_channels[i] = NULL;
    free(c);
}
static void coc_reset_locked(void)
{
    for (size_t i = 0; i < COC_CHANNELS; i++) if (coc_channels[i]) coc_free(coc_channels[i]);
    coc_server_registered = false;
}
static size_t coc_put(coc_fifo_t *q, const uint8_t *data, size_t len)
{
    if (len > COC_BUFFER_SIZE - q->size) len = COC_BUFFER_SIZE - q->size;
    size_t tail = (q->head + q->size) % COC_BUFFER_SIZE;
    size_t first = len < COC_BUFFER_SIZE - tail ? len : COC_BUFFER_SIZE - tail;
    memcpy(q->data + tail, data, first);
    memcpy(q->data, data + first, len - first);
    q->size += len;
    return len;
}
static size_t coc_get(coc_fifo_t *q, uint8_t *data, size_t len)
{
    if (len > q->size) len = q->size;
    size_t first = len < COC_BUFFER_SIZE - q->head ? len : COC_BUFFER_SIZE - q->head;
    memcpy(data, q->data + q->head, first);
    memcpy(data + first, q->data, len - first);
    q->head = (q->head + len) % COC_BUFFER_SIZE;
    q->size -= len;
    return len;
}
static void coc_receive_ready(coc_channel_t *c)
{
    if (!c->chan || c->rx_armed || c->failed || c->closing ||
        COC_BUFFER_SIZE - c->rx.size < COC_MTU) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(NULL, 0);
    if (!om) return; /* The next I/O/status poll queues a retry. */
    int rc = ble_l2cap_recv_ready(c->chan, om);
    if (rc) { os_mbuf_free_chain(om); c->failed = true; }
    else c->rx_armed = true;
}
static int coc_event(struct ble_l2cap_event *event, void *arg)
{
    lock();
    coc_channel_t *c = arg ? coc_find((uint32_t)(uintptr_t)arg) : NULL;
    struct ble_l2cap_chan *chan = NULL;
    switch (event->type) {
    case BLE_L2CAP_EVENT_COC_ACCEPT:
        /* Only the owner holding the generic peripheral lease may accept.
         * A second peer/channel cannot enter an existing byte session. */
        for (size_t i = 0; i < COC_CHANNELS; i++) {
            coc_channel_t *candidate = coc_channels[i];
            if (candidate && candidate->listener && candidate->started &&
                !candidate->closing && !candidate->failed && !candidate->chan &&
                peripheral && peripheral->owner == candidate->owner &&
                !peripheral->closing && server_peer(0, event->accept.conn_handle)) {
                c = candidate; break;
            }
        }
        if (!c) { unlock(); return BLE_HS_EBUSY; }
        c->conn = event->accept.conn_handle;
        c->chan = event->accept.chan;
        c->connecting = true;
        coc_receive_ready(c);
        if (!c->rx_armed) {
            c->chan = NULL; c->connecting = false;
            unlock(); return BLE_HS_ENOMEM;
        }
        /* Retain one peer for this claim. A new offer belongs to a fresh
         * consumer session, after the current owner releases its stream. */
        peripheral->wants_advertising = false;
        if (peripheral->advertising) {
            (void)ble_gap_adv_stop();
            peripheral->advertising = false;
        }
        unlock(); return 0;
    case BLE_L2CAP_EVENT_COC_CONNECTED: chan = event->connect.chan; break;
    case BLE_L2CAP_EVENT_COC_DISCONNECTED: chan = event->disconnect.chan; break;
    case BLE_L2CAP_EVENT_COC_DATA_RECEIVED: chan = event->receive.chan; break;
    case BLE_L2CAP_EVENT_COC_TX_UNSTALLED: chan = event->tx_unstalled.chan; break;
    default: unlock(); return 0;
    }
    if (!arg && chan) for (size_t i = 0; i < COC_CHANNELS; i++)
        if (coc_channels[i] && coc_channels[i]->chan == chan) { c = coc_channels[i]; break; }
    if (!c || (c->chan && c->chan != chan)) {
        if (event->type == BLE_L2CAP_EVENT_COC_DATA_RECEIVED)
            os_mbuf_free_chain(event->receive.sdu_rx);
        unlock(); return 0;
    }
    if (event->type == BLE_L2CAP_EVENT_COC_CONNECTED) {
        c->connecting = false;
        c->failed = event->connect.status != 0;
        c->chan = c->failed ? NULL : chan;
        c->connected = !c->failed;
        c->refresh_info = c->connected;
        if (c->connected) {
            struct ble_l2cap_chan_info info = {0};
            if (ble_l2cap_get_chan_info(chan, &info) || !info.peer_coc_mtu) c->failed = true;
            else c->tx_mtu = info.peer_coc_mtu < COC_MTU ? info.peer_coc_mtu : COC_MTU;
            /* DLE is per connection and per direction. Both ends request full
             * packets; unsupported controllers retain their current settings. */
            (void)ble_gap_set_data_len(c->conn, 251, 2120);
            if (!c->listener) {
                /* Prefer a responsive stream without changing the keyboard's
                 * connection parameters. Rejection leaves a usable channel. */
                struct ble_gap_upd_params params = {
                    .itvl_min=12, .itvl_max=12, .latency=0, .supervision_timeout=400,
                    .min_ce_len=0, .max_ce_len=24};
                if (ble_gap_update_params(c->conn, &params)) {
                    /* Some controllers reject a short interval alongside a
                     * keyboard. Try 25 ms, retaining the current timing if
                     * that is rejected too. */
                    params.itvl_min = params.itvl_max = 20;
                    (void)ble_gap_update_params(c->conn, &params);
                }
                (void)ble_gap_set_prefered_le_phy(c->conn,
                    BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
            }
        }
    } else if (event->type == BLE_L2CAP_EVENT_COC_DISCONNECTED) {
        c->chan = NULL; c->connecting = c->connected = c->rx_armed = false;
        c->failed = true;
    } else if (event->type == BLE_L2CAP_EVENT_COC_DATA_RECEIVED) {
        struct os_mbuf *om = event->receive.sdu_rx;
        size_t len = OS_MBUF_PKTLEN(om);
        c->rx_armed = false;
        if (len > COC_MTU || len > COC_BUFFER_SIZE - c->rx.size) c->failed = true;
        else if (!c->closing) {
            uint8_t data[COC_MTU];
            if (os_mbuf_copydata(om, 0, len, data)) c->failed = true;
            else (void)coc_put(&c->rx, data, len);
        }
        os_mbuf_free_chain(om);
        coc_receive_ready(c);
    } else {
        c->stalled = false;
        if (event->tx_unstalled.status) c->failed = true;
    }
    if (c->closing && !c->chan && !c->connecting) coc_free(c);
    coc_queue();
    unlock(); return 0;
}

static void coc_commands_locked(void)
{
    for (size_t i = 0; i < COC_CHANNELS; i++) {
        coc_channel_t *c = coc_channels[i];
        if (!c) continue;
        if (c->closing || c->failed) {
            if (c->chan) {
                /* GAP teardown by the owning service is also a retirement
                 * barrier if the channel disconnect request cannot be sent. */
                (void)ble_l2cap_disconnect(c->chan);
            } else if (c->closing && !c->connecting) coc_free(c);
            continue;
        }
        if (!c->started) {
            c->started = true;
            if (c->listener) {
                if (!coc_server_registered) {
                    int rc = ble_l2cap_create_server(SOLAR_OS_BLE_STREAM_PSM, COC_MTU, coc_event, NULL);
                    c->failed = rc != 0;
                    coc_server_registered = !rc;
                }
            } else {
                struct os_mbuf *om = ble_hs_mbuf_from_flat(NULL, 0);
                if (!om) { c->failed = true; continue; }
                c->connecting = true; c->rx_armed = true;
                /* Installed NimBLE consumes this mbuf on every valid call,
                 * including immediate connect failure. */
                int rc = ble_l2cap_connect(c->conn, SOLAR_OS_BLE_STREAM_PSM,
                    COC_MTU, om, coc_event, (void *)(uintptr_t)c->token);
                if (rc) { c->connecting = false; c->failed = true; c->rx_armed = false; }
            }
        }
        if (!c->connected || c->failed) continue;
        if (c->refresh_info) {
            struct ble_gap_conn_desc desc = {0};
            if (!ble_gap_conn_find(c->conn, &desc))
                c->connection_interval_us = (uint32_t)desc.conn_itvl * 1250;
            c->refresh_info = false;
        }
        coc_receive_ready(c);
        unsigned burst = 0;
        for (; c->tx.size && !c->stalled && !c->failed && burst < 8; burst++) {
            size_t len = c->tx.size < c->tx_mtu ? c->tx.size : c->tx_mtu;
            uint8_t data[COC_MTU];
            /* Peek, so allocation pressure cannot lose accepted bytes. */
            coc_fifo_t *q = &c->tx;
            for (size_t j = 0; j < len; j++) data[j] = q->data[(q->head + j) % COC_BUFFER_SIZE];
            struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
            if (!om) break;
            int rc = ble_l2cap_send(c->chan, om);
            /* ESTALLED transfers ownership and must never be retransmitted.
             * EBUSY/EBADDATA reject before consumption; other errors in the
             * installed SDK free the SDU after a partial transmit attempt. */
            if (rc == BLE_HS_EBUSY || rc == BLE_HS_EBADDATA) os_mbuf_free_chain(om);
            if (!rc || rc == BLE_HS_ESTALLED) {
                (void)coc_get(q, data, len);
                c->stalled = rc == BLE_HS_ESTALLED;
            } else c->failed = true;
        }
        if (burst == 8 && c->tx.size && !c->stalled && !c->failed) coc_queue();
    }
}

uint16_t solar_os_ble_backend_coc_mtu(void) { return COC_MTU; }
esp_err_t solar_os_ble_backend_coc_open(solar_os_ble_session_t owner,
    uint16_t conn, bool listener, uint32_t *token)
{
    if (!owner || !token || !mutex || !command_ready) return ESP_ERR_INVALID_STATE;
    lock();
    bool valid = listener ? peripheral && peripheral->owner == owner && !peripheral->closing : false;
    if (!listener) for (ble_client_t *p = clients; p; p = p->next)
        if (p->conn == conn && !p->retiring) valid = true;
    if (!valid) { unlock(); return ESP_ERR_INVALID_STATE; }
    for (size_t i = 0; i < COC_CHANNELS; i++)
        if (listener && coc_channels[i] && coc_channels[i]->listener && !coc_channels[i]->closing) {
            unlock(); return ESP_ERR_INVALID_STATE;
        }
    size_t slot = 0;
    while (slot < COC_CHANNELS && coc_channels[slot]) slot++;
    if (slot == COC_CHANNELS || coc_ticket == UINT32_MAX) { unlock(); return ESP_ERR_NO_MEM; }
    coc_channel_t *c = calloc(1, sizeof(*c));
    if (!c) { unlock(); return ESP_ERR_NO_MEM; }
    c->token = ++coc_ticket; c->owner = owner; c->conn = conn; c->listener = listener;
    coc_channels[slot] = c; *token = c->token;
    coc_queue(); unlock(); return ESP_OK;
}
void solar_os_ble_backend_coc_close(uint32_t token)
{
    if (!mutex) return;
    lock();
    coc_channel_t *c = coc_find(token);
    if (c) { c->closing = true; coc_queue(); }
    unlock();
}
esp_err_t solar_os_ble_backend_coc_status(uint32_t token, solar_os_ble_coc_info_t *info)
{
    if (!info || !mutex) return ESP_ERR_INVALID_ARG;
    lock();
    coc_channel_t *c = coc_find(token);
    esp_err_t err = c && !c->closing ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (!err) *info = (solar_os_ble_coc_info_t){.connected=c->connected && !c->failed,
        .failed=c->failed, .rx_mtu=COC_MTU, .tx_mtu=c->tx_mtu,
        .connection_interval_us=c->connection_interval_us,
        .rx_pending=c->rx.size, .tx_pending=c->tx.size};
    if (c && !c->closing && !c->failed) { c->refresh_info = true; coc_queue(); }
    unlock(); return err;
}
esp_err_t solar_os_ble_backend_coc_read(uint32_t token, uint8_t *data, size_t len, size_t *received)
{
    if (!received || (!data && len) || !mutex) return ESP_ERR_INVALID_ARG;
    *received = 0;
    lock();
    coc_channel_t *c = coc_find(token);
    esp_err_t err = !c || c->failed || c->closing ? ESP_FAIL : ESP_OK;
    if (!err && len) {
        *received = coc_get(&c->rx, data, len);
        if (!*received) err = ESP_ERR_TIMEOUT;
    }
    if (c && !c->closing && !c->failed) coc_queue();
    unlock(); return err;
}
esp_err_t solar_os_ble_backend_coc_write(uint32_t token, const uint8_t *data, size_t len, size_t *written)
{
    if (!written || (!data && len) || !mutex) return ESP_ERR_INVALID_ARG;
    *written = 0;
    lock();
    coc_channel_t *c = coc_find(token);
    esp_err_t err = !c || c->failed || c->closing ? ESP_FAIL : ESP_OK;
    /* Accept bounded output while establishing the channel too. A listening
     * shell writes its banner before the central arrives; these bytes belong
     * to this claim and must survive until that peer connects. */
    if (!err && len) *written = coc_put(&c->tx, data, len);
    if (c && !c->closing && !c->failed) coc_queue();
    unlock(); return err;
}
