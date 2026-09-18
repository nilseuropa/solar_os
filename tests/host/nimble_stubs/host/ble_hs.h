#pragma once
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 4
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_EMSGSIZE 4
#define BLE_HS_ENOENT 5
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_EAPP 9
#define BLE_HS_EBUSY 15
#define BLE_HS_EDONE 14
#define BLE_HS_ETIMEOUT 13
#define BLE_HS_ENOTSYNCED 22
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_ERR_AUTH_FAIL 0x05
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
#define BLE_GAP_REPEAT_PAIRING_IGNORE 2
#define BLE_GAP_SUBSCRIBE_REASON_WRITE 1
#define BLE_GAP_SUBSCRIBE_REASON_TERM 2
#define BLE_GAP_SUBSCRIBE_REASON_RESTORE 3
#define BLE_OWN_ADDR_PUBLIC 0
#define BLE_UUID_TYPE_16 16
#define BLE_UUID_TYPE_32 32
#define BLE_UUID_TYPE_128 128
#define BLE_GATT_CHR_PROP_NOTIFY 0x10
#define BLE_GATT_CHR_PROP_INDICATE 0x20
#define BLE_GATT_CHR_PROP_READ 0x02
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 0x04
typedef struct { uint8_t type; uint8_t val[6]; } ble_addr_t;
typedef struct { uint8_t type; } ble_uuid_t;
typedef struct { ble_uuid_t u; uint16_t value; } ble_uuid16_t;
typedef struct { ble_uuid_t u; uint32_t value; } ble_uuid32_t;
typedef struct { ble_uuid_t u; uint8_t value[16]; } ble_uuid128_t;
typedef union { ble_uuid_t u; ble_uuid16_t u16; ble_uuid32_t u32; ble_uuid128_t u128; uint8_t raw[20]; } ble_uuid_any_t;
#define BLE_UUID16(u) ((const ble_uuid16_t *)(u))
#define BLE_UUID32(u) ((const ble_uuid32_t *)(u))
static inline uint16_t ble_uuid_u16(const ble_uuid_t *u) { return u->type == 16 ? BLE_UUID16(u)->value : 0; }
char *ble_uuid_to_str(const ble_uuid_t *uuid, char *out);
struct ble_gatt_svc { uint16_t start_handle, end_handle; ble_uuid_any_t uuid; };
struct ble_gatt_chr { uint16_t def_handle, val_handle; uint8_t properties; ble_uuid_any_t uuid; };
struct ble_gatt_dsc { uint16_t handle; ble_uuid_any_t uuid; };
struct ble_gatt_error { int status; };
struct os_mbuf { size_t len; uint8_t *data; struct os_mbuf *next; };
size_t nimble_test_mbuf_len(const struct os_mbuf *om);
#define OS_MBUF_PKTLEN(om) nimble_test_mbuf_len(om)
int os_mbuf_copydata(const struct os_mbuf *om, int offset, int len, void *out);
struct ble_gatt_attr { uint16_t handle; struct os_mbuf *om; };
struct ble_gap_sec_state { unsigned encrypted:1, authenticated:1, bonded:1, key_size:5, authorize:1; };
struct ble_gap_conn_desc { struct ble_gap_sec_state sec_state; ble_addr_t peer_id_addr; uint8_t role; };
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *desc);
enum { BLE_GAP_EVENT_CONNECT, BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_ENC_CHANGE, BLE_GAP_EVENT_NOTIFY_RX,
    BLE_GAP_EVENT_ADV_COMPLETE, BLE_GAP_EVENT_SUBSCRIBE, BLE_GAP_EVENT_NOTIFY_TX,
    BLE_GAP_EVENT_PASSKEY_ACTION, BLE_GAP_EVENT_REPEAT_PAIRING };
struct ble_gap_event {
    int type;
    union {
        struct { int status; uint16_t conn_handle; } connect;
        struct { int reason; struct { uint16_t conn_handle; } conn; } disconnect;
        struct { int status; uint16_t conn_handle; } enc_change;
        struct { struct { uint8_t action; uint32_t numcmp; } params; uint16_t conn_handle; } passkey;
        struct { uint16_t conn_handle; } repeat_pairing;
        struct { uint16_t conn_handle, attr_handle; struct os_mbuf *om; bool indication; } notify_rx;
        struct {
            uint16_t conn_handle, attr_handle;
            uint8_t reason;
            bool cur_notify, cur_indicate;
        } subscribe;
        struct { uint16_t conn_handle, attr_handle; int status; bool indication; } notify_tx;
    };
};
typedef int ble_gap_event_fn(struct ble_gap_event *, void *);
typedef int ble_gatt_disc_svc_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_svc *, void *);
typedef int ble_gatt_chr_fn(uint16_t, const struct ble_gatt_error *, const struct ble_gatt_chr *, void *);
typedef int ble_gatt_dsc_fn(uint16_t, const struct ble_gatt_error *, uint16_t, const struct ble_gatt_dsc *, void *);
typedef int ble_gatt_attr_fn(uint16_t, const struct ble_gatt_error *, struct ble_gatt_attr *, void *);
typedef int ble_gatt_mtu_fn(uint16_t, const struct ble_gatt_error *, uint16_t, void *);
int ble_gap_connect(uint8_t, const ble_addr_t *, int, const void *, ble_gap_event_fn *, void *);
int ble_gap_conn_cancel(void);
int ble_gap_terminate(uint16_t, uint8_t);
int ble_gap_security_initiate(uint16_t);
int ble_gattc_disc_all_svcs(uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_find_inc_svcs(uint16_t, uint16_t, uint16_t, ble_gatt_disc_svc_fn *, void *);
int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, ble_gatt_chr_fn *, void *);
int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, ble_gatt_dsc_fn *, void *);
int ble_gattc_exchange_mtu(uint16_t, ble_gatt_mtu_fn *, void *);
uint16_t ble_att_mtu(uint16_t);
int ble_gattc_read(uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_read_long(uint16_t, uint16_t, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_flat(uint16_t, uint16_t, const void *, uint16_t, ble_gatt_attr_fn *, void *);
int ble_gattc_write_no_rsp_flat(uint16_t, uint16_t, const void *, uint16_t);

#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_HS_FOREVER INT32_MAX
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_GATT_ACCESS_OP_READ_CHR 0
#define BLE_GATT_ACCESS_OP_WRITE_CHR 1
#define BLE_GATT_ACCESS_OP_READ_DSC 2
#define BLE_ATT_ERR_UNLIKELY 14
#define BLE_ATT_ERR_INVALID_OFFSET 7
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 13
#define BLE_ATT_ERR_INSUFFICIENT_RES 17
#define BLE_GATT_CHR_F_READ 2
#define BLE_GATT_CHR_F_WRITE_NO_RSP 4
#define BLE_GATT_CHR_F_WRITE 8
#define BLE_GATT_CHR_F_NOTIFY 16
#define BLE_GATT_CHR_F_INDICATE 32
#define BLE_GATT_CHR_F_READ_ENC 0x00000200
#define BLE_GATT_CHR_F_WRITE_ENC 0x00001000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC 0x00008000
#define BLE_ATT_F_READ 0x01
#define BLE_ATT_F_READ_ENC 0x04
#define BLE_GATT_SVC_TYPE_PRIMARY 1
struct ble_gatt_access_ctxt { uint8_t op; struct os_mbuf *om; uint16_t offset; };
struct ble_gatt_dsc_def {
    const ble_uuid_t *uuid;
    uint8_t att_flags;
    uint8_t min_key_size;
    int (*access_cb)(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
    void *arg;
};
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    uint16_t flags;
    int (*access_cb)(uint16_t, uint16_t, struct ble_gatt_access_ctxt *, void *);
    void *arg;
    struct ble_gatt_dsc_def *descriptors;
    uint16_t *val_handle;
};
struct ble_gatt_svc_def { uint8_t type; const ble_uuid_t *uuid; const struct ble_gatt_chr_def *characteristics; };
struct ble_hs_adv_fields {
    uint8_t flags, num_uuids16, num_uuids32, num_uuids128;
    ble_uuid16_t *uuids16; ble_uuid32_t *uuids32; ble_uuid128_t *uuids128;
    uint8_t *name; size_t name_len; bool name_is_complete;
    uint16_t appearance; bool appearance_is_present;
    bool uuids16_is_complete;
};
struct ble_gap_adv_params { uint8_t conn_mode, disc_mode; };
int ble_uuid_from_str(ble_uuid_any_t *, const char *);
int ble_uuid_cmp(const ble_uuid_t *, const ble_uuid_t *);
int ble_gap_adv_stop(void);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *);
int ble_gap_adv_start(uint8_t, const ble_addr_t *, int32_t, const struct ble_gap_adv_params *, ble_gap_event_fn *, void *);
int ble_gatts_add_dynamic_svcs(const struct ble_gatt_svc_def *);
int ble_gatts_delete_svc(const ble_uuid_t *);
int ble_gatts_find_svc(const ble_uuid_t *, uint16_t *);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *, uint16_t);
int ble_gatts_notify_custom(uint16_t, uint16_t, struct os_mbuf *);
int ble_gatts_indicate_custom(uint16_t, uint16_t, struct os_mbuf *);
int os_mbuf_append(struct os_mbuf *, const void *, uint16_t);
