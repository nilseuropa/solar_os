#pragma once
#include "host/ble_hs.h"
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
int ble_store_read_cccd(const struct ble_store_key_cccd *key,
                        struct ble_store_value_cccd *out_value);
int ble_store_write_cccd(const struct ble_store_value_cccd *value);
int ble_store_util_delete_peer(const ble_addr_t *peer_id_addr);
