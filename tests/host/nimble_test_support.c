#include "nimble_test_support.h"
#include "esp_hid_common.h"
#include "host/ble_store.h"
#include "freertos/queue.h"
#include <nvs.h>
#include "host/ble_hs_id.h"
#include <freertos/semphr.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>

struct nimble_test_state fake;
struct ble_hs_cfg_stub ble_hs_cfg;
static struct ble_npl_eventq eventq;
static pthread_mutex_t event_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event_changed = PTHREAD_COND_INITIALIZER;
void nimble_test_reset(void)
{ memset(&fake, 0, sizeof(fake)); fake.mtu = 517;
  ble_hs_cfg=(struct ble_hs_cfg_stub){.sm_io_cap=BLE_HS_IO_DISPLAY_ONLY,.sm_mitm=1,.sm_sc=1}; }
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void) { return &eventq; }
void ble_npl_event_init(struct ble_npl_event *e, void (*fn)(struct ble_npl_event *), void *arg)
{ *e = (struct ble_npl_event){.fn = fn, .arg = arg}; }
void ble_npl_eventq_put(struct ble_npl_eventq *q, struct ble_npl_event *e)
{
    pthread_mutex_lock(&event_lock);
    if (!e->queued) { assert(q->count < 32); q->items[q->count++] = e; e->queued = true; }
    pthread_cond_broadcast(&event_changed); pthread_mutex_unlock(&event_lock);
}
void nimble_test_wait_event(void)
{
    struct timespec deadline; clock_gettime(CLOCK_REALTIME,&deadline); deadline.tv_sec+=2;
    pthread_mutex_lock(&event_lock);
    while (!eventq.count) assert(!pthread_cond_timedwait(&event_changed,&event_lock,&deadline));
    pthread_mutex_unlock(&event_lock);
}
void nimble_test_drain(void)
{
    unsigned budget = 100;
    for (;;) {
        pthread_mutex_lock(&event_lock);
        if (!eventq.count) { pthread_mutex_unlock(&event_lock); break; }
        assert(budget--);
        struct ble_npl_event *e = eventq.items[0];
        memmove(eventq.items, eventq.items + 1, --eventq.count * sizeof(e));
        e->queued = false; pthread_mutex_unlock(&event_lock); e->fn(e);
    }
}
int ble_npl_callout_init(struct ble_npl_callout *c, struct ble_npl_eventq *q,
    void (*fn)(struct ble_npl_event *), void *arg)
{ (void)q; memset(c, 0, sizeof(*c)); ble_npl_event_init(&c->event, fn, arg); return 0; }
int ble_npl_callout_reset(struct ble_npl_callout *c, uint32_t ticks) { c->active = true; c->ticks = ticks; return 0; }
void ble_npl_callout_stop(struct ble_npl_callout *c) { c->active = false; }
void ble_npl_callout_deinit(struct ble_npl_callout *c) { assert(!c->active); }
void ble_npl_event_deinit(struct ble_npl_event *e) { assert(!e->queued); }
int ble_gap_connect(uint8_t own, const ble_addr_t *addr, int ms, const void *params,
    ble_gap_event_fn *fn, void *arg)
{ (void)own; (void)params; assert(ms == 3000); fake.address = *addr; fake.connect_calls++;
  fake.gap = fn; fake.gap_arg = arg; return fake.submit_error; }
int ble_gap_conn_cancel(void) { fake.cancel_calls++; return 0; }
int ble_gap_terminate(uint16_t c, uint8_t r) { (void)c; (void)r; fake.terminate_calls++; return 0; }
int ble_gap_security_initiate(uint16_t c)
{
    (void)c; fake.security_calls++;
    fake.security_mitm=ble_hs_cfg.sm_mitm;
    fake.security_sc=ble_hs_cfg.sm_sc;
    return fake.submit_error;
}
int ble_sm_inject_io(uint16_t c, struct ble_sm_io *io)
{ (void)c; fake.inject_calls++; fake.injected=*io; return fake.submit_error; }
int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *desc)
{ (void)c; desc->peer_id_addr=fake.address; desc->sec_state.encrypted=fake.encrypted;
  desc->sec_state.bonded=fake.bonded; return 0; }
int ble_store_util_delete_peer(const ble_addr_t *peer_id_addr)
{
    fake.store_delete_calls++;
    fake.deleted_bond = *peer_id_addr;
    if (fake.store_delete_error) return fake.store_delete_error;
    if (!memcmp(&fake.store_cccd_written.peer_addr, peer_id_addr, sizeof(*peer_id_addr)))
        memset(&fake.store_cccd_written, 0, sizeof(fake.store_cccd_written));
    for (size_t type=0; type<2; type++) {
        for (size_t i=0; i<fake.store_bond_count[type];) {
            if (!memcmp(&fake.store_bonds[type][i],peer_id_addr,sizeof(*peer_id_addr))) {
                memmove(&fake.store_bonds[type][i],&fake.store_bonds[type][i+1],
                    (--fake.store_bond_count[type]-i)*sizeof(*peer_id_addr));
            } else i++;
        }
    }
    return 0;
}
int ble_store_iterate(int type, ble_store_iterator_fn *callback, void *arg)
{
    if (fake.store_iterate_error) return fake.store_iterate_error;
    if (type==BLE_STORE_OBJ_TYPE_CCCD) {
        if (fake.store_cccd_written.chr_val_handle) {
            union ble_store_value value={.cccd=fake.store_cccd_written};
            (void)callback(type,&value,arg);
        }
        return 0;
    }
    assert(type==BLE_STORE_OBJ_TYPE_OUR_SEC || type==BLE_STORE_OBJ_TYPE_PEER_SEC);
    size_t index=type==BLE_STORE_OBJ_TYPE_OUR_SEC ? 0 : 1;
    for (size_t i=0; i<fake.store_bond_count[index]; i++) {
        union ble_store_value value={.sec={.peer_addr=fake.store_bonds[index][i]}};
        if (callback(type,&value,arg)) break;
    }
    return 0;
}
int ble_gap_conn_find_by_addr(const ble_addr_t *address, struct ble_gap_conn_desc *desc)
{
    for (size_t i=0; i<fake.active_bond_count; i++) {
        if (!memcmp(address,&fake.active_bonds[i],sizeof(*address))) {
            if (desc) desc->peer_id_addr=*address;
            return 0;
        }
    }
    return BLE_HS_ENOTCONN;
}
bool solar_os_ble_nimble_keyboard_bond_remembered(const ble_addr_t *address)
{
    return fake.keyboard_bond_valid &&
        !memcmp(address,&fake.keyboard_bond,sizeof(*address));
}
int ble_store_read_cccd(const struct ble_store_key_cccd *key,
                        struct ble_store_value_cccd *value)
{
    fake.store_cccd_read_calls++;
    if (!fake.store_cccd_handle ||
        key->chr_val_handle != fake.store_cccd_handle) return BLE_HS_ENOENT;
    *value = (struct ble_store_value_cccd){
        .peer_addr = key->peer_addr,
        .chr_val_handle = key->chr_val_handle,
        .flags = fake.store_cccd_flags,
    };
    return 0;
}
int ble_store_write_cccd(const struct ble_store_value_cccd *value)
{
    fake.store_cccd_write_calls++;
    fake.store_cccd_written = *value;
    return 0;
}
int ble_gattc_exchange_mtu(uint16_t c, ble_gatt_mtu_fn *fn, void *arg)
{ (void)c; fake.mtu_fn = fn; fake.arg = arg; return fake.submit_error; }
uint16_t ble_att_mtu(uint16_t c) { (void)c; return fake.mtu; }
int ble_gattc_disc_all_svcs(uint16_t c, ble_gatt_disc_svc_fn *fn, void *arg)
{ (void)c; fake.svc = fn; fake.arg = arg; return fake.submit_error; }
int ble_gattc_find_inc_svcs(uint16_t c, uint16_t s, uint16_t e, ble_gatt_disc_svc_fn *fn, void *arg)
{ (void)c; fake.last_start=s; fake.last_end=e; fake.included=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_disc_all_chrs(uint16_t c, uint16_t s, uint16_t e, ble_gatt_chr_fn *fn, void *arg)
{ (void)c; fake.last_start=s; fake.last_end=e; fake.chr=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_disc_all_dscs(uint16_t c, uint16_t s, uint16_t e, ble_gatt_dsc_fn *fn, void *arg)
{ (void)c; fake.last_start=s; fake.last_end=e; fake.dsc=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_read(uint16_t c, uint16_t h, ble_gatt_attr_fn *fn, void *arg)
{ (void)c; fake.read_calls++; fake.last_handle=h; fake.attr=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_read_long(uint16_t c, uint16_t h, uint16_t offset, ble_gatt_attr_fn *fn, void *arg)
{ assert(offset == 0); return ble_gattc_read(c,h,fn,arg); }
int ble_gattc_read_by_uuid(uint16_t c, uint16_t s, uint16_t e, const ble_uuid_t *uuid,
                          ble_gatt_attr_fn *fn, void *arg)
{ (void)c; fake.read_uuid_calls++; fake.last_start=s; fake.last_end=e;
  fake.read_uuid=ble_uuid_u16(uuid); fake.attr=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_write_flat(uint16_t c, uint16_t h, const void *v, uint16_t n, ble_gatt_attr_fn *fn, void *arg)
{ (void)c; fake.write_calls++; fake.last_handle=h; assert(n<=sizeof(fake.written));
  memcpy(fake.written,v,n); fake.written_len=n; fake.attr=fn; fake.arg=arg; return fake.submit_error; }
int ble_gattc_write_no_rsp_flat(uint16_t c, uint16_t h, const void *v, uint16_t n)
{ return ble_gattc_write_flat(c,h,v,n,NULL,NULL); }
size_t nimble_test_mbuf_len(const struct os_mbuf *m)
{ size_t n=0; for (; m; m=m->next) n+=m->len; return n; }
int os_mbuf_copydata(const struct os_mbuf *m, int offset, int len, void *out)
{
    uint8_t *p=out;
    for (; m && len; m=m->next) {
        if ((size_t)offset >= m->len) { offset-=m->len; continue; }
        size_t n=m->len-offset; if (n>(size_t)len) n=len;
        memcpy(p,m->data+offset,n); p+=n; len-=n; offset=0;
    }
    return len ? -1 : 0;
}
char *ble_uuid_to_str(const ble_uuid_t *u, char *out)
{ (void)u; strcpy(out,"3ad10001-8d42-4e59-9282-5aaf9d312001"); return out; }
void nimble_test_connect(int status)
{ struct ble_gap_event e={.type=BLE_GAP_EVENT_CONNECT,.connect={.status=status,.conn_handle=7}};
  fake.gap(&e,fake.gap_arg); }
void nimble_test_disconnect(void)
{ struct ble_gap_event e={.type=BLE_GAP_EVENT_DISCONNECT,.disconnect={.reason=19,.conn={.conn_handle=7}}};
  fake.gap(&e,fake.gap_arg); }
void nimble_test_value(int status, uint16_t handle, uint8_t *value, size_t len)
{ struct os_mbuf m={.len=len,.data=value}; struct ble_gatt_attr a={.handle=handle,.om=&m};
  struct ble_gatt_error err={.status=status}; fake.attr(7,&err,&a,fake.arg); }

int ble_uuid_from_str(ble_uuid_any_t *uuid, const char *text)
{
    memset(uuid, 0, sizeof(*uuid));
    if (strlen(text) == 4) { uuid->u.type = 16; uuid->u16.value = strtoul(text, NULL, 16); }
    else if (strlen(text) == 8) { uuid->u.type = 32; uuid->u32.value = strtoul(text, NULL, 16); }
    else { uuid->u.type = 128; memcpy(uuid->u128.value, text, 16); }
    return 0;
}
int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b)
{
    if (a->type != b->type) return a->type - b->type;
    if (a->type == 16) return BLE_UUID16(a)->value - BLE_UUID16(b)->value;
    if (a->type == 32) return BLE_UUID32(a)->value != BLE_UUID32(b)->value;
    return memcmp(((const ble_uuid128_t *)a)->value, ((const ble_uuid128_t *)b)->value, 16);
}
int ble_gap_adv_stop(void) { fake.advertising = false; return 0; }
int ble_gap_wl_set(const ble_addr_t *peers, uint8_t count)
{ assert(count==1); fake.whitelist_calls++; fake.whitelist_peer=*peers; return fake.whitelist_error; }
int ble_svc_gap_device_name_set(const char *name)
{ assert(strlen(name) <= 26); strcpy(fake.device_name, name); return fake.submit_error; }
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *fields)
{ fake.adv_flags=fields->flags; fake.appearance = fields->appearance_is_present ? fields->appearance : 0; return fake.submit_error; }
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *fields) { assert(fields->name_len <= 26); return fake.submit_error; }
int ble_gap_adv_start(uint8_t own, const ble_addr_t *addr, int32_t ms,
    const struct ble_gap_adv_params *params, ble_gap_event_fn *cb, void *arg)
{
    (void)ms; fake.adv_own=own; fake.adv_mode=params->conn_mode; fake.adv_filter=params->filter_policy;
    fake.adv_target=addr ? *addr : (ble_addr_t){0};
    fake.adv_calls++; fake.server_gap=cb; fake.server_gap_arg=arg;
    fake.advertising = !fake.submit_error; return fake.submit_error;
}
int ble_gatts_add_dynamic_svcs(const struct ble_gatt_svc_def *definitions)
{
    fake.server_add_calls++;
    if (fake.server_add_error) return fake.server_add_error;
    fake.server_definitions = definitions;
    uint16_t handle = 100;
    for (const struct ble_gatt_svc_def *s = definitions; s->type; s++)
        for (const struct ble_gatt_chr_def *c = s->characteristics; c && c->uuid; c++) {
            *c->val_handle = handle; handle += 3;
        }
    return 0;
}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *definitions)
{ (void)definitions; return fake.server_count_error; }
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *definitions)
{
    fake.server_static_add_calls++;
    const int calls = fake.server_add_calls;
    int rc = ble_gatts_add_dynamic_svcs(definitions);
    fake.server_add_calls = calls;
    return rc;
}
int ble_gatts_delete_svc(const ble_uuid_t *uuid)
{ (void)uuid; fake.server_delete_calls++; return fake.server_delete_error; }
int ble_gatts_find_svc(const ble_uuid_t *uuid, uint16_t *handle)
{ (void)uuid; (void)handle; return BLE_HS_ENOENT; }
struct os_mbuf *ble_hs_mbuf_from_flat(const void *data, uint16_t len)
{
    if (fake.mbuf_fail) return NULL;
    struct os_mbuf *om = calloc(1, sizeof(*om)); assert(om);
    om->data = malloc(len ? len : 1); assert(om->data); om->len = len; memcpy(om->data, data, len); return om;
}
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len)
{
    if (fake.mbuf_fail) return BLE_HS_ENOMEM;
    om->data = realloc(om->data, om->len + len + 1); assert(om->data);
    memcpy(om->data + om->len, data, len); om->len += len; return 0;
}
int ble_gatts_notify_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om)
{
    (void)conn; fake.notify_calls++; fake.last_handle = handle;
    assert(om->len <= sizeof(fake.written)); memcpy(fake.written, om->data, om->len); fake.written_len = om->len;
    free(om->data); free(om); return fake.submit_error;
}
int ble_gatts_indicate_custom(uint16_t conn, uint16_t handle, struct os_mbuf *om)
{ return ble_gatts_notify_custom(conn, handle, om); }

struct test_queue { size_t count, cap, size; uint8_t data[]; };
QueueHandle_t xQueueCreate(size_t n, size_t size)
{ struct test_queue *q=calloc(1,sizeof(*q)+n*size); assert(q); q->cap=n; q->size=size; return q; }
int xQueueSend(QueueHandle_t q, const void *value, unsigned timeout)
{ (void)timeout; if(q->count==q->cap) return pdFALSE;
  memcpy(q->data+q->size*q->count++,value,q->size); return pdTRUE; }
int xQueueReceive(QueueHandle_t q, void *value, unsigned timeout)
{ (void)timeout; if(!q->count) return pdFALSE; memcpy(value,q->data,q->size);
  memmove(q->data,q->data+q->size,--q->count*q->size); return pdTRUE; }
unsigned uxQueueSpacesAvailable(QueueHandle_t q) { return q->cap-q->count; }

int ble_store_read_our_sec(const struct ble_store_key_sec *key, struct ble_store_value_sec *value)
{
    for (size_t i=0; i<fake.store_bond_count[0]; i++)
        if (!memcmp(&key->peer_addr, &fake.store_bonds[0][i], sizeof(ble_addr_t))) {
            *value=(struct ble_store_value_sec){.peer_addr=key->peer_addr,
                .ltk_present=!fake.bond_no_ltk,
                .authenticated=fake.bond_authenticated,.sc=fake.bond_sc};
            return 0;
        }
    return BLE_HS_ENOENT;
}
int ble_gap_conn_active(void) { return fake.initiating; }
int ble_gap_disc_active(void) { return fake.scan_active; }
int ble_gap_disc_cancel(void) { fake.scan_cancel_calls++; fake.scan_active=false; return 0; }
int ble_hs_id_gen_rnd(int nrpa, ble_addr_t *address)
{ assert(!nrpa); *address=(ble_addr_t){.type=1,.val={++fake.random_calls,2,3,4,5,0xc6}}; return 0; }
int ble_hs_id_set_rnd(const uint8_t *address)
{ fake.random_set_calls++; memcpy(fake.random_address,address,6); return fake.submit_error; }
int ble_hs_id_copy_addr(uint8_t type, uint8_t *out, int *nrpa)
{ assert(type==0); (void)nrpa; memcpy(out,(uint8_t[]){1,2,3,4,5,6},6); return 0; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ assert(!strcmp(name,"hidhosts")); *handle=1; return fake.nvs_error ? fake.nvs_error : mode==NVS_READONLY && !fake.nvs_length ? ESP_ERR_NVS_NOT_FOUND : ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{ (void)handle; assert(!strcmp(key,"hosts")); if (!fake.nvs_length) return ESP_ERR_NVS_NOT_FOUND;
  if (*length<fake.nvs_length) return ESP_ERR_INVALID_SIZE;
  *length=fake.nvs_length; memcpy(out,fake.nvs_blob,*length); return ESP_OK; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t length)
{ (void)handle; assert(!strcmp(key,"hosts") && length<=sizeof(fake.nvs_blob));
  memcpy(fake.nvs_pending,data,length); fake.nvs_pending_length=length; return fake.nvs_error; }
esp_err_t nvs_commit(nvs_handle_t handle)
{ (void)handle; if(fake.nvs_error) return fake.nvs_error;
  memcpy(fake.nvs_blob,fake.nvs_pending,fake.nvs_pending_length); fake.nvs_length=fake.nvs_pending_length; return ESP_OK; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
