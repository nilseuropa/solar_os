#pragma once
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
extern struct nimble_test_state {
    int submit_error, connect_calls, cancel_calls, terminate_calls, security_calls, read_calls, write_calls;
    uint16_t mtu, last_handle, last_start, last_end;
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
    uint8_t written[128];
    size_t written_len;
    int server_add_error, server_delete_error, server_add_calls, server_delete_calls, adv_calls, notify_calls;
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
