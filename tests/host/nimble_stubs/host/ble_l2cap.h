#pragma once
#include "ble_hs.h"
#define MYNEWT_VAL(name) MYNEWT_VAL_##name
#define MYNEWT_VAL_BLE_L2CAP_COC_MPS 248
#define BLE_HS_ESTALLED 23
#define BLE_HS_EBADDATA 24
enum { BLE_L2CAP_EVENT_COC_CONNECTED, BLE_L2CAP_EVENT_COC_DISCONNECTED,
    BLE_L2CAP_EVENT_COC_ACCEPT, BLE_L2CAP_EVENT_COC_DATA_RECEIVED,
    BLE_L2CAP_EVENT_COC_TX_UNSTALLED };
struct ble_l2cap_chan;
struct ble_gap_upd_params {
    uint16_t itvl_min, itvl_max, latency, supervision_timeout, min_ce_len, max_ce_len;
};
int ble_gap_update_params(uint16_t, const struct ble_gap_upd_params *);
#define BLE_GAP_LE_PHY_2M_MASK 2
int ble_gap_set_data_len(uint16_t, uint16_t, uint16_t);
int ble_gap_set_prefered_le_phy(uint16_t, uint8_t, uint8_t, uint16_t);
struct ble_l2cap_event {
    uint8_t type;
    union {
        struct { int status; uint16_t conn_handle; struct ble_l2cap_chan *chan; } connect;
        struct { uint16_t conn_handle; struct ble_l2cap_chan *chan; } disconnect;
        struct { uint16_t conn_handle, peer_sdu_size; struct ble_l2cap_chan *chan; } accept;
        struct { uint16_t conn_handle; struct ble_l2cap_chan *chan; struct os_mbuf *sdu_rx; } receive;
        struct { uint16_t conn_handle; struct ble_l2cap_chan *chan; int status; } tx_unstalled;
    };
};
struct ble_l2cap_chan_info { uint16_t our_coc_mtu, peer_coc_mtu; };
typedef int ble_l2cap_event_fn(struct ble_l2cap_event *, void *);
int ble_l2cap_create_server(uint16_t, uint16_t, ble_l2cap_event_fn *, void *);
int ble_l2cap_connect(uint16_t, uint16_t, uint16_t, struct os_mbuf *, ble_l2cap_event_fn *, void *);
int ble_l2cap_disconnect(struct ble_l2cap_chan *);
int ble_l2cap_send(struct ble_l2cap_chan *, struct os_mbuf *);
int ble_l2cap_recv_ready(struct ble_l2cap_chan *, struct os_mbuf *);
int ble_l2cap_get_chan_info(struct ble_l2cap_chan *, struct ble_l2cap_chan_info *);
void os_mbuf_free_chain(struct os_mbuf *);
