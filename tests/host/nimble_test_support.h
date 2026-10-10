#pragma once
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "solar_os_ble.h"
extern struct nimble_test_state {
    int submit_error, connect_calls, cancel_calls, terminate_calls, security_calls, inject_calls, read_calls, write_calls;
    uint16_t mtu, last_handle, last_start, last_end, read_uuid;
    int read_uuid_calls;
    ble_addr_t address;
    ble_gap_event_fn *gap;
    void *gap_arg;
    ble_gatt_mtu_fn *mtu_fn;
    ble_gatt_disc_svc_fn *svc;
    ble_gatt_disc_svc_fn *included;
    ble_gatt_chr_fn *chr;
    ble_gatt_dsc_fn *dsc;
    ble_gatt_attr_fn *attr;
    void *arg;
    uint8_t written[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t written_len;
    struct ble_sm_io injected;
    int server_add_error, server_delete_error, server_add_calls, server_delete_calls, adv_calls, notify_calls;
    int server_static_add_calls, server_count_error;
    int store_delete_error, store_delete_calls;
    int store_iterate_error;
    ble_addr_t store_bonds[2][4], active_bonds[4], keyboard_bond, deleted_bond;
    size_t store_bond_count[2], active_bond_count;
    bool keyboard_bond_valid;
    bool bond_no_ltk, bond_authenticated, bond_sc;
    uint8_t security_mitm, security_sc;
    int store_cccd_read_calls, store_cccd_write_calls;
    uint16_t store_cccd_handle, store_cccd_flags;
    struct ble_store_value_cccd store_cccd_written;
    int random_calls, random_set_calls, scan_cancel_calls, nvs_error;
    bool scan_active, initiating;
    uint8_t random_address[6], adv_own, adv_mode, adv_filter, adv_flags;
    int whitelist_calls, whitelist_error;
    ble_addr_t adv_target, whitelist_peer;
    uint8_t nvs_blob[256], nvs_pending[256];
    size_t nvs_length, nvs_pending_length;
    bool advertising, mbuf_fail, encrypted, bonded;
    uint16_t appearance;
    char device_name[27];
    ble_gap_event_fn *server_gap;
    void *server_gap_arg;
    const struct ble_gatt_svc_def *server_definitions;
} fake;
void nimble_test_reset(void);
void nimble_test_wait_event(void);
void nimble_test_connect(int status);
void nimble_test_disconnect(void);
void nimble_test_value(int status, uint16_t handle, uint8_t *value, size_t length);
