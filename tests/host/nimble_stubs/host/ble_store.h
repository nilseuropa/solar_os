#pragma once
#include "host/ble_hs.h"
#define BLE_STORE_OBJ_TYPE_OUR_SEC 1
#define BLE_STORE_OBJ_TYPE_PEER_SEC 2
#define BLE_STORE_OBJ_TYPE_CCCD 3
#define BLE_STORE_EVENT_OVERFLOW 1
#define BLE_STORE_EVENT_FULL 2
union ble_store_value;
struct ble_store_status_event {
    int event_code;
    struct { int obj_type; uint16_t conn_handle; } full;
};
typedef int ble_store_iterator_fn(int type, union ble_store_value *value, void *arg);
int ble_store_iterate(int type, ble_store_iterator_fn *callback, void *arg);
struct ble_store_key_sec { ble_addr_t peer_addr; };
struct ble_store_value_sec {
    ble_addr_t peer_addr;
    uint8_t ltk_present, authenticated, sc;
};
int ble_store_read_our_sec(const struct ble_store_key_sec *, struct ble_store_value_sec *);
struct ble_store_key_cccd {
    ble_addr_t peer_addr;
    uint16_t chr_val_handle;
    uint8_t idx;
};
struct ble_store_value_cccd {
    ble_addr_t peer_addr;
    uint16_t chr_val_handle;
    uint16_t flags;
    unsigned value_changed:1;
};
union ble_store_value {
    struct { ble_addr_t peer_addr; } sec;
    struct ble_store_value_cccd cccd;
};
int ble_store_read_cccd(const struct ble_store_key_cccd *key,
                        struct ble_store_value_cccd *out_value);
int ble_store_write_cccd(const struct ble_store_value_cccd *value);
int ble_store_util_delete_peer(const ble_addr_t *peer_id_addr);
