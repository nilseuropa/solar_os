/* NimBLE peripheral transport, included in the adapter to share its host queue
 * and lock. All stack calls run on that queue. Public requests/events are copied
 * and contain no NimBLE pointers; service dispatch pins owner/host lifetime. */
enum {
    SERVER_KIND_GENERIC,
    SERVER_KIND_HID,
};
enum {
    SERVER_HID_NONE,
    SERVER_HID_INFORMATION,
    SERVER_HID_REPORT_MAP,
    SERVER_HID_CONTROL_POINT,
    SERVER_HID_KEYBOARD_INPUT,
    SERVER_HID_KEYBOARD_OUTPUT,
    SERVER_HID_MOUSE_INPUT,
    SERVER_HID_GAMEPAD_INPUT,
};
typedef struct server_desc {
    struct server_desc *next;
    ble_uuid_any_t uuid;
    uint8_t flags;
    size_t len;
    uint8_t value[8];
} server_desc_t;
typedef struct server_char {
    struct server_char *next;
    ble_uuid_any_t uuid;
    uint32_t id;
    uint16_t handle;
    size_t index;
    uint32_t flags;
    uint8_t hid_kind;
    size_t len;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    const uint8_t *static_value;
    server_desc_t *descs;
    struct ble_gatt_dsc_def *descriptor_definitions;
} server_char_t;
typedef struct server_service {
    struct server_service *next;
    ble_uuid_any_t uuid;
    uint32_t id;
    server_char_t *chars;
    struct ble_gatt_chr_def *definitions;
} server_service_t;
typedef struct server_peer {
    struct server_peer *next;
    uint32_t id;
    uint16_t conn, indication_handle;
    bool retiring, encrypted, bonded;
    uint8_t subscriptions[]; /* Indexed by immutable local characteristic ID. */
} server_peer_t;
typedef struct {
    uint32_t owner, epoch;
    uint8_t kind;
    bool registered, closing, dormant, advertising, wants_advertising, neutralized,
        pairing;
    char name[27];
    server_service_t *services;
    server_peer_t *peers;
    server_peer_t *pending_peer; /* Reserved before advertising; connect callback cannot allocate. */
    size_t service_count, char_count, peer_count, untracked;
    struct ble_gatt_svc_def *definitions;
    solar_os_ble_server_event_t *events;
    solar_os_ble_hid_device_event_t *hid_events;
    size_t capacity, head, count;
    uint32_t dropped;
    uint32_t hid_keyboard, hid_mouse, hid_gamepad;
    ble_addr_t hid_bond_addr;
    bool hid_bond_valid;
    uint8_t hid_subscriptions[3];
    uint8_t keyboard[8], keyboard_leds, mouse_buttons;
    int16_t gamepad_axes[6];
    uint32_t gamepad_buttons;
    uint8_t gamepad_hat;
} server_state_t;
typedef struct {
    uint32_t owner;
    unsigned refs;
    bool done, abandoned;
    esp_err_t result;
    StaticSemaphore_t storage;
    SemaphoreHandle_t wake;
    bool hid;
    union {
        solar_os_ble_server_request_t server_request;
        solar_os_ble_hid_request_t hid_request;
    } request;
} server_command_t;
static server_state_t *peripheral;
static server_command_t *server_command;
static uint32_t server_peer_ticket;
static uint32_t server_epoch_ticket;
static uint32_t server_local_ticket;
static int server_gap(struct ble_gap_event *event, void *arg);
static void server_hid_neutralize(void);

/* Composite report map matching SolarOS's typed USB HID reports. Report IDs
 * identify separate HOGP Report characteristics; notification payloads do not
 * include the ID byte. */
static const uint8_t server_hid_report_map[] = {
    /* Keyboard, report ID 1. */
    0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,
    0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x08,0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x01,
    0x05,0x08,0x19,0x01,0x29,0x05,0x75,0x01,0x95,0x05,0x91,0x02,
    0x75,0x03,0x95,0x01,0x91,0x01,
    0x05,0x07,0x19,0x00,0x2a,0xff,0x00,0x15,0x00,0x26,0xff,0x00,
    0x75,0x08,0x95,0x06,0x81,0x00,0xc0,
    /* Mouse, report ID 2. */
    0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x02,0x09,0x01,0xa1,0x00,
    0x05,0x09,0x19,0x01,0x29,0x05,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x05,0x81,0x02,0x75,0x03,0x95,0x01,0x81,0x01,
    0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,
    0x75,0x08,0x95,0x03,0x81,0x06,
    0x05,0x0c,0x0a,0x38,0x02,0x15,0x81,0x25,0x7f,
    0x75,0x08,0x95,0x01,0x81,0x06,0xc0,0xc0,
    /* Gamepad, report ID 3. */
    0x05,0x01,0x09,0x05,0xa1,0x01,0x85,0x03,
    0x09,0x30,0x09,0x31,0x09,0x32,0x09,0x35,0x09,0x33,0x09,0x34,
    0x15,0x81,0x25,0x7f,0x75,0x08,0x95,0x06,0x81,0x02,
    0x09,0x39,0x15,0x01,0x25,0x08,0x35,0x00,0x46,0x3b,0x01,
    0x65,0x14,0x75,0x08,0x95,0x01,0x81,0x02,
    0x05,0x09,0x19,0x01,0x29,0x20,0x15,0x00,0x25,0x01,
    0x75,0x01,0x95,0x20,0x81,0x02,0xc0,
};

static size_t server_used_locked(void)
{
    return peripheral ? peripheral->peer_count + peripheral->untracked +
        (peripheral->advertising ? 1 : 0) : 0;
}
static bool server_idle_locked(void)
{
    return (!peripheral || peripheral->dormant) && !server_command;
}
static size_t server_central_count(void)
{
    size_t count = 0;
    for (ble_client_t *c = clients; c; c = c->next) count++;
    return count;
}
static server_char_t *server_characteristic(uint32_t id, uint16_t handle)
{
    if (peripheral) for (server_service_t *s = peripheral->services; s; s = s->next)
        for (server_char_t *c = s->chars; c; c = c->next)
            if ((id && c->id == id) || (handle && c->handle == handle)) return c;
    return NULL;
}
static server_peer_t *server_peer(uint32_t id, uint16_t conn)
{
    if (peripheral) for (server_peer_t *p = peripheral->peers; p; p = p->next)
        if ((id && p->id == id) || (!id && p->conn == conn)) return p;
    return NULL;
}
static void server_drop(void)
{
    if (peripheral->dropped != UINT32_MAX) peripheral->dropped++;
}
static bool server_event(solar_os_ble_server_event_t *event)
{
    if (!peripheral || peripheral->closing) return false;
    if (peripheral->count == peripheral->capacity) { server_drop(); return false; }
    size_t index = (peripheral->head + peripheral->count++) % peripheral->capacity;
    peripheral->events[index] = *event;
    return true;
}
static bool server_hid_event(solar_os_ble_hid_device_event_t *event)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID || peripheral->closing)
        return false;
    if (peripheral->count == peripheral->capacity) { server_drop(); return false; }
    size_t index = (peripheral->head + peripheral->count++) % peripheral->capacity;
    peripheral->hid_events[index] = *event;
    return true;
}
static int server_hid_subscription_slot(uint8_t kind)
{
    switch (kind) {
    case SERVER_HID_KEYBOARD_INPUT: return 0;
    case SERVER_HID_MOUSE_INPUT: return 1;
    case SERVER_HID_GAMEPAD_INPUT: return 2;
    default: return -1;
    }
}
static bool server_same_address(const ble_addr_t *left, const ble_addr_t *right)
{
    return left && right && left->type == right->type &&
        !memcmp(left->val, right->val, sizeof(left->val));
}
static void server_store_hid_subscription(const server_peer_t *peer,
                                          const server_char_t *characteristic,
                                          const struct ble_gap_conn_desc *desc)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID || !peer ||
        !characteristic || !desc || !desc->sec_state.bonded) return;
    const int slot = server_hid_subscription_slot(characteristic->hid_kind);
    if (slot < 0) return;
    const uint8_t flags = peer->subscriptions[characteristic->index] & 0x03U;
    peripheral->hid_bond_addr = desc->peer_id_addr;
    peripheral->hid_bond_valid = true;
    peripheral->hid_subscriptions[slot] = flags;
    const struct ble_store_value_cccd value = {
        .peer_addr = desc->peer_id_addr,
        .chr_val_handle = characteristic->handle,
        .flags = flags,
    };
    (void)ble_store_write_cccd(&value);
}
static void server_restore_hid_subscriptions(server_peer_t *peer,
                                             const struct ble_gap_conn_desc *desc)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID || !peer || !desc ||
        !desc->sec_state.bonded) return;
    const bool cache_matches = peripheral->hid_bond_valid &&
        server_same_address(&peripheral->hid_bond_addr, &desc->peer_id_addr);
    for (server_service_t *service = peripheral->services; service;
         service = service->next) {
        for (server_char_t *characteristic = service->chars; characteristic;
             characteristic = characteristic->next) {
            if (!characteristic->handle ||
                (characteristic->hid_kind != SERVER_HID_KEYBOARD_INPUT &&
                 characteristic->hid_kind != SERVER_HID_MOUSE_INPUT &&
                 characteristic->hid_kind != SERVER_HID_GAMEPAD_INPUT)) continue;
            const struct ble_store_key_cccd key = {
                .peer_addr = desc->peer_id_addr,
                .chr_val_handle = characteristic->handle,
            };
            struct ble_store_value_cccd value = {0};
            if (!ble_store_read_cccd(&key, &value)) {
                /* NimBLE stores notify and indicate in the low two bits. Keep
                 * the server mirror in sync even when the bonded host relies
                 * on its persisted CCCD instead of writing it again. */
                peer->subscriptions[characteristic->index] =
                    (uint8_t)(value.flags & 0x03U);
            } else if (cache_matches) {
                const int slot =
                    server_hid_subscription_slot(characteristic->hid_kind);
                if (slot >= 0) {
                    peer->subscriptions[characteristic->index] =
                        peripheral->hid_subscriptions[slot];
                }
            }
            server_store_hid_subscription(peer, characteristic, desc);
        }
    }
}
static void server_free(void)
{
    if (!peripheral) return;
    while (peripheral->services) {
        server_service_t *s = peripheral->services;
        peripheral->services = s->next;
        while (s->chars) {
            server_char_t *c = s->chars;
            s->chars = c->next;
            while (c->descs) {
                server_desc_t *d = c->descs;
                c->descs = d->next;
                free(d);
            }
            free(c->descriptor_definitions);
            free(c);
        }
        free(s->definitions); free(s);
    }
    while (peripheral->peers) {
        server_peer_t *p = peripheral->peers; peripheral->peers = p->next; free(p);
    }
    free(peripheral->pending_peer);
    free(peripheral->definitions); free(peripheral->events);
    free(peripheral->hid_events); free(peripheral); peripheral = NULL;
}
static void server_command_release(server_command_t *c)
{
    if (!--c->refs) { vSemaphoreDelete(c->wake); free(c); }
}
static void server_reset_locked(void)
{
    /* Host stopped and callbacks drained; NimBLE no longer retains definitions. */
    server_free();
    if (server_command) {
        server_command_t *c = server_command; server_command = NULL;
        c->done = true; c->result = SOLAR_OS_BLE_ERR_CANCELLED;
        xSemaphoreGive(c->wake); server_command_release(c);
    }
}
static int server_unregister_services(void)
{
    if (!peripheral || !peripheral->registered) return 0;
    for (server_service_t *service = peripheral->services; service;
         service = service->next) {
        int rc = ble_gatts_delete_svc(&service->uuid.u);
        if (rc && rc != BLE_HS_ENOENT) return rc;
    }
    peripheral->registered = false;
    return 0;
}
static void server_retire(void)
{
    if (!peripheral || !peripheral->closing) return;
    peripheral->wants_advertising = false;
    if (peripheral->advertising) {
        int rc = ble_gap_adv_stop();
        if (rc && rc != BLE_HS_EALREADY) return;
        peripheral->advertising = false;
    }
    if (peripheral->kind == SERVER_KIND_HID && !peripheral->neutralized) {
        server_hid_neutralize();
    }
    for (server_peer_t *p = peripheral->peers; p; p = p->next) {
        if (!p->retiring && !ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM)) p->retiring = true;
    }
    if (peripheral->peer_count || peripheral->untracked) return;
    if (peripheral->kind == SERVER_KIND_HID) {
        /* Bonded HOGP clients persist CCCDs by ATT handle. Keep the inactive
         * native service registered so a later script lease reuses those
         * handles and reconnects without pairing again. */
        peripheral->owner = SOLAR_OS_BLE_SESSION_INVALID;
        peripheral->closing = false;
        peripheral->dormant = true;
        peripheral->pairing = false;
        peripheral->head = peripheral->count = 0;
        peripheral->dropped = 0;
        return;
    }
    if (server_unregister_services()) return; /* Retain definitions on teardown failure. */
    server_free();
}
static int server_advertise(void)
{
    if (!peripheral || peripheral->closing || peripheral->dormant ||
        !peripheral->wants_advertising ||
        peripheral->advertising ||
        (peripheral->kind == SERVER_KIND_HID && peripheral->peer_count)) return 0;
    if (server_central_count() + peripheral->peer_count + peripheral->untracked >=
        solar_os_ble_backend_capacity()) return BLE_HS_EBUSY;
    if (server_peer_ticket == UINT32_MAX) return BLE_HS_ENOMEM;
    if (!peripheral->pending_peer) {
        peripheral->pending_peer = calloc(1, sizeof(server_peer_t) + peripheral->char_count + 1);
        if (!peripheral->pending_peer) return BLE_HS_ENOMEM;
    }
    int rc = ble_svc_gap_device_name_set(peripheral->name);
    if (rc) return rc;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    if (peripheral->kind == SERVER_KIND_HID) {
        fields.appearance = 0x03c0; /* Generic Human Interface Device. */
        fields.appearance_is_present = 1;
    }
    ble_uuid_any_t *uuid = &peripheral->services->uuid;
    if (uuid->u.type == BLE_UUID_TYPE_16) {
        fields.uuids16 = &uuid->u16; fields.num_uuids16 = 1;
        fields.uuids16_is_complete = 1;
    } else if (uuid->u.type == BLE_UUID_TYPE_32) {
        fields.uuids32 = &uuid->u32; fields.num_uuids32 = 1;
    } else {
        fields.uuids128 = &uuid->u128; fields.num_uuids128 = 1;
    }
    rc = ble_gap_adv_set_fields(&fields);
    if (rc) return rc;
    memset(&fields, 0, sizeof(fields));
    fields.name = (uint8_t *)peripheral->name;
    fields.name_len = strlen(peripheral->name); fields.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&fields);
    if (rc) return rc;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND; params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params,
        server_gap, (void *)(uintptr_t)peripheral->epoch);
    peripheral->advertising = rc == 0;
    return rc;
}
static int server_access(uint16_t conn, uint16_t handle, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    lock();
    server_char_t *c = arg;
    server_peer_t *p = server_peer(0, conn);
    if (!peripheral || peripheral->closing || !p || p->retiring) { unlock(); return BLE_ATT_ERR_UNLIKELY; }
    solar_os_ble_server_event_t event = {.peer=p->id, .characteristic=c->id, .mtu=ble_att_mtu(conn)};
    int rc = 0;
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const uint8_t *value = c->static_value ? c->static_value : c->value;
        /* NimBLE applies Read Blob offsets after the access callback. */
        if (ctx->offset > c->len) rc = BLE_ATT_ERR_INVALID_OFFSET;
        else if (os_mbuf_append(ctx->om, value, c->len)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        else if (peripheral->kind == SERVER_KIND_GENERIC) {
            event.type = SOLAR_OS_BLE_SERVER_READ; (void)server_event(&event);
        }
    } else if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        event.type = SOLAR_OS_BLE_SERVER_WRITE; event.value_len = OS_MBUF_PKTLEN(ctx->om);
        if (ctx->offset) rc = BLE_ATT_ERR_INVALID_OFFSET;
        else if (event.value_len > sizeof(event.value)) rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        else if (os_mbuf_copydata(ctx->om, 0, event.value_len, event.value)) rc = BLE_ATT_ERR_UNLIKELY;
        else if (peripheral->kind == SERVER_KIND_HID) {
            if ((c->hid_kind == SERVER_HID_KEYBOARD_OUTPUT ||
                 c->hid_kind == SERVER_HID_CONTROL_POINT) && event.value_len != 1) {
                rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            } else {
                if (c->hid_kind == SERVER_HID_KEYBOARD_OUTPUT) {
                    const uint8_t keyboard_leds = event.value[0] & 0x1f;
                    solar_os_ble_hid_device_event_t hid_event = {
                        .type = SOLAR_OS_BLE_HID_KEYBOARD_LEDS,
                        .peer = p->id,
                        .keyboard_leds = keyboard_leds,
                        .encrypted = p->encrypted,
                        .bonded = p->bonded,
                    };
                    if (!server_hid_event(&hid_event)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
                    else peripheral->keyboard_leds = keyboard_leds;
                }
                if (!rc) {
                    memcpy(c->value, event.value, event.value_len);
                    c->len = event.value_len;
                }
            }
        } else if (!server_event(&event)) rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        else { memcpy(c->value, event.value, event.value_len); c->len = event.value_len; }
    } else rc = BLE_ATT_ERR_UNLIKELY;
    (void)handle;
    unlock();
    return rc;
}
static int server_descriptor_access(uint16_t conn, uint16_t handle,
                                    struct ble_gatt_access_ctxt *ctx, void *arg)
{
    lock();
    server_desc_t *d = arg;
    server_peer_t *p = server_peer(0, conn);
    int rc = 0;
    if (!peripheral || peripheral->closing || !p || p->retiring ||
        ctx->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        rc = BLE_ATT_ERR_UNLIKELY;
    } else if (ctx->offset > d->len) {
        rc = BLE_ATT_ERR_INVALID_OFFSET;
    } else if (os_mbuf_append(ctx->om, d->value, d->len)) {
        rc = BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    (void)handle;
    unlock();
    return rc;
}
static int server_repeat_pairing(struct ble_gap_event *event)
{
    struct ble_gap_conn_desc desc = {0};
    if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc)) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    if (ble_store_util_delete_peer(&desc.peer_id_addr)) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}
static int server_gap(struct ble_gap_event *event, void *arg)
{
    lock();
    if (!peripheral || peripheral->epoch != (uint32_t)(uintptr_t)arg) { unlock(); return 0; }
    solar_os_ble_server_event_t e = {0};
    server_peer_t *p = NULL;
    bool delegate_security = false;
    bool hid_passkey = false;
    bool hid_repeat_pairing = false;
    uint32_t security_epoch = 0;
    uint32_t security_peer = SOLAR_OS_BLE_PEER_INVALID;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        peripheral->advertising = false;
        if (!event->connect.status) {
            p = peripheral->pending_peer; peripheral->pending_peer = NULL;
            if (!p) {
                peripheral->untracked++;
                server_drop();
                (void)ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            p->id = ++server_peer_ticket; p->conn = event->connect.conn_handle;
            p->next = peripheral->peers; peripheral->peers = p; peripheral->peer_count++;
            if (peripheral->closing || server_central_count() + peripheral->peer_count +
                peripheral->untracked > solar_os_ble_backend_capacity()) {
                p->retiring = !ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
            } else if (peripheral->kind == SERVER_KIND_HID) {
                peripheral->neutralized = false;
                solar_os_ble_hid_device_event_t hid_event = {
                    .type = SOLAR_OS_BLE_HID_CONNECTED,
                    .peer = p->id,
                };
                (void)server_hid_event(&hid_event);
                int rc = 0;
                if (peripheral->pairing) {
                    struct ble_gap_conn_desc desc = {0};
                    rc = ble_gap_conn_find(p->conn, &desc);
                    if (!rc) {
                        rc = ble_store_util_delete_peer(&desc.peer_id_addr);
                        if (rc == BLE_HS_ENOENT) rc = 0;
                    }
                }
                if (!rc) rc = ble_gap_security_initiate(p->conn);
                if (rc && rc != BLE_HS_EALREADY) {
                    p->retiring = !ble_gap_terminate(p->conn, BLE_ERR_AUTH_FAIL);
                }
            } else {
                e.type = SOLAR_OS_BLE_SERVER_CONNECTED; e.peer = p->id; e.mtu = ble_att_mtu(p->conn);
                (void)server_event(&e);
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        p = server_peer(0, event->disconnect.conn.conn_handle);
        if (p) {
            if (peripheral->kind == SERVER_KIND_HID) {
                solar_os_ble_hid_device_event_t hid_event = {
                    .type = SOLAR_OS_BLE_HID_DISCONNECTED,
                    .peer = p->id,
                    .status = event->disconnect.reason,
                };
                (void)server_hid_event(&hid_event);
                memset(peripheral->keyboard, 0, sizeof(peripheral->keyboard));
                peripheral->mouse_buttons = 0;
                memset(peripheral->gamepad_axes, 0, sizeof(peripheral->gamepad_axes));
                peripheral->gamepad_buttons = 0;
                peripheral->gamepad_hat = 0;
            } else {
                e.type = SOLAR_OS_BLE_SERVER_DISCONNECTED; e.peer = p->id;
                e.status = event->disconnect.reason;
                (void)server_event(&e);
            }
            server_peer_t **entry = &peripheral->peers;
            while (*entry && *entry != p) entry = &(*entry)->next;
            if (*entry) *entry = p->next;
            peripheral->peer_count--; free(p);
        } else if (peripheral->untracked) peripheral->untracked--;
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        p = server_peer(0, event->enc_change.conn_handle);
        if (p && peripheral->kind == SERVER_KIND_HID) {
            struct ble_gap_conn_desc desc = {0};
            const int rc = event->enc_change.status ? event->enc_change.status :
                ble_gap_conn_find(p->conn, &desc);
            p->encrypted = !rc && desc.sec_state.encrypted;
            p->bonded = !rc && desc.sec_state.bonded;
            if (p->bonded) peripheral->pairing = false;
            if (!rc) server_restore_hid_subscriptions(p, &desc);
            solar_os_ble_hid_device_event_t hid_event = {
                .type = SOLAR_OS_BLE_HID_SECURED,
                .peer = p->id,
                .status = rc,
                .encrypted = p->encrypted,
                .bonded = p->bonded,
            };
            (void)server_hid_event(&hid_event);
            if (rc || !p->encrypted) {
                p->retiring = !ble_gap_terminate(p->conn, BLE_ERR_AUTH_FAIL);
            }
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        peripheral->advertising = false;
        /* Unexpected controller stop is observable; do not spin-retry errors. */
        peripheral->wants_advertising = false;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        p = server_peer(0, event->subscribe.conn_handle);
        server_char_t *c = server_characteristic(0, event->subscribe.attr_handle);
        if (p && c) {
            /* Connection teardown reports every CCCD as disabled. Those are
             * transient link cleanup events, not host preference changes;
             * retaining them would overwrite the bonded HID subscriptions
             * that the next application lease needs to restore. */
            if (peripheral->kind == SERVER_KIND_HID &&
                event->subscribe.reason == BLE_GAP_SUBSCRIBE_REASON_TERM) {
                break;
            }
            p->subscriptions[c->index] = (event->subscribe.cur_notify ? 1 : 0) |
                (event->subscribe.cur_indicate ? 2 : 0);
            if (peripheral->kind == SERVER_KIND_HID) {
                struct ble_gap_conn_desc desc = {0};
                if (!ble_gap_conn_find(p->conn, &desc)) {
                    server_store_hid_subscription(p, c, &desc);
                }
            } else {
                e.type = SOLAR_OS_BLE_SERVER_SUBSCRIBE; e.peer = p->id; e.characteristic = c->id;
                e.notify = event->subscribe.cur_notify; e.indicate = event->subscribe.cur_indicate;
                (void)server_event(&e);
            }
        }
        break;
    }
    case BLE_GAP_EVENT_NOTIFY_TX: {
        p = server_peer(0, event->notify_tx.conn_handle);
        server_char_t *c = server_characteristic(0, event->notify_tx.attr_handle);
        if (p && c && peripheral->kind == SERVER_KIND_GENERIC &&
            (!event->notify_tx.indication || event->notify_tx.status)) {
            if (event->notify_tx.indication) p->indication_handle = 0;
            e.type = SOLAR_OS_BLE_SERVER_SENT; e.peer = p->id; e.characteristic = c->id;
            e.indicate = event->notify_tx.indication;
            e.status = event->notify_tx.status == BLE_HS_EDONE ? 0 : event->notify_tx.status;
            (void)server_event(&e);
        }
        break;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (peripheral->kind == SERVER_KIND_HID) {
            p = server_peer(0, event->passkey.conn_handle);
            if (p) {
                hid_passkey = true;
                security_epoch = peripheral->epoch;
                security_peer = p->id;
            } else {
                (void)ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
            }
        } else delegate_security = true;
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        if (peripheral->kind == SERVER_KIND_HID &&
            server_peer(0, event->repeat_pairing.conn_handle)) {
            hid_repeat_pairing = true;
        } else delegate_security = true;
        break;
    default: break;
    }
    /* Defer registration, advertising restart and deletion out of GAP callbacks. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    unlock();
    if (hid_passkey) {
        uint32_t passkey = UINT32_MAX;
        const int rc = solar_os_ble_nimble_security_passkey(event, &passkey);
        if (rc || passkey == UINT32_MAX) return rc;
        lock();
        p = peripheral && peripheral->epoch == security_epoch &&
            security_peer != SOLAR_OS_BLE_PEER_INVALID ?
            server_peer(security_peer, event->passkey.conn_handle) : NULL;
        if (p) {
            solar_os_ble_hid_device_event_t hid_event = {
                .type = SOLAR_OS_BLE_HID_PASSKEY,
                .peer = p->id,
                .passkey = passkey,
            };
            if (!server_hid_event(&hid_event)) {
                p->retiring = !ble_gap_terminate(p->conn, BLE_ERR_AUTH_FAIL);
            }
        }
        unlock();
        return 0;
    }
    if (hid_repeat_pairing) return server_repeat_pairing(event);
    return delegate_security ? solar_os_ble_nimble_security(event) : 0;
}
static bool server_uuid(const char *text, ble_uuid_any_t *uuid)
{
    size_t n = strlen(text);
    if (n == 6 && text[0] == '0' && text[1] == 'x') { text += 2; n -= 2; }
    if (n != 4 && n != 8 && n != 36) return false;
    for (size_t i = 0; i < n; i++) {
        if (n == 36 && (i == 8 || i == 13 || i == 18 || i == 23)) {
            if (text[i] != '-') return false;
        } else if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f') ||
                     (text[i] >= 'A' && text[i] <= 'F'))) return false;
    }
    return ble_uuid_from_str(uuid, text) == 0;
}
static int server_register_services(void)
{
    if (peripheral->registered) return 0;
    if (!peripheral->service_count) return BLE_HS_EINVAL;
    free(peripheral->definitions);
    peripheral->definitions = calloc(peripheral->service_count + 1, sizeof(*peripheral->definitions));
    if (!peripheral->definitions) return BLE_HS_ENOMEM;
    size_t index = 0;
    for (server_service_t *s = peripheral->services; s; s = s->next, index++) {
        uint16_t existing;
        if (!ble_gatts_find_svc(&s->uuid.u, &existing)) return BLE_HS_EALREADY;
        size_t count = 0;
        for (server_char_t *c = s->chars; c; c = c->next) count++;
        free(s->definitions); s->definitions = calloc(count + 1, sizeof(*s->definitions));
        if (!s->definitions) return BLE_HS_ENOMEM;
        peripheral->definitions[index] = (struct ble_gatt_svc_def){
            .type=BLE_GATT_SVC_TYPE_PRIMARY, .uuid=&s->uuid.u, .characteristics=s->definitions};
        size_t ci = 0;
        for (server_char_t *c = s->chars; c; c = c->next, ci++) {
            size_t descriptors = 0;
            for (server_desc_t *d = c->descs; d; d = d->next) descriptors++;
            free(c->descriptor_definitions);
            c->descriptor_definitions = NULL;
            if (descriptors) {
                c->descriptor_definitions = calloc(descriptors + 1,
                                                    sizeof(*c->descriptor_definitions));
                if (!c->descriptor_definitions) return BLE_HS_ENOMEM;
                size_t di = 0;
                for (server_desc_t *d = c->descs; d; d = d->next, di++) {
                    c->descriptor_definitions[di] = (struct ble_gatt_dsc_def){
                        .uuid = &d->uuid.u,
                        .att_flags = d->flags,
                        .access_cb = server_descriptor_access,
                        .arg = d,
                    };
                }
            }
            s->definitions[ci] = (struct ble_gatt_chr_def){
                .uuid=&c->uuid.u, .flags=c->flags, .access_cb=server_access,
                .arg=c, .descriptors=c->descriptor_definitions,
                .val_handle=&c->handle};
        }
    }
    int rc = ble_gatts_add_dynamic_svcs(peripheral->definitions);
    peripheral->registered = rc == 0;
    return rc;
}

static server_service_t *server_add_service_internal(const char *uuid_text)
{
    if (!peripheral || server_local_ticket == UINT32_MAX) return NULL;
    server_service_t *service = calloc(1, sizeof(*service));
    if (!service || !server_uuid(uuid_text, &service->uuid)) {
        free(service);
        return NULL;
    }
    service->id = ++server_local_ticket;
    server_service_t **entry = &peripheral->services;
    while (*entry) entry = &(*entry)->next;
    *entry = service;
    peripheral->service_count++;
    return service;
}

static server_char_t *server_add_char_internal(server_service_t *service,
                                               const char *uuid_text,
                                               uint32_t flags,
                                               uint8_t hid_kind,
                                               const uint8_t *value,
                                               size_t len,
                                               bool static_value)
{
    if (!service || server_local_ticket == UINT32_MAX ||
        (!static_value && len > SOLAR_OS_BLE_GATT_VALUE_MAX)) return NULL;
    server_char_t *c = calloc(1, sizeof(*c));
    if (!c || !server_uuid(uuid_text, &c->uuid)) { free(c); return NULL; }
    c->id = ++server_local_ticket;
    c->index = peripheral->char_count++;
    c->flags = flags;
    c->hid_kind = hid_kind;
    c->len = len;
    if (static_value) c->static_value = value;
    else if (len) memcpy(c->value, value, len);
    server_char_t **entry = &service->chars;
    while (*entry) entry = &(*entry)->next;
    *entry = c;
    return c;
}

static bool server_add_desc_internal(server_char_t *c,
                                     const char *uuid_text,
                                     uint8_t flags,
                                     const uint8_t *value,
                                     size_t len)
{
    if (!c || !value || len > sizeof(((server_desc_t *)0)->value)) return false;
    server_desc_t *d = calloc(1, sizeof(*d));
    if (!d || !server_uuid(uuid_text, &d->uuid)) { free(d); return false; }
    d->flags = flags; d->len = len; memcpy(d->value, value, len);
    server_desc_t **entry = &c->descs;
    while (*entry) entry = &(*entry)->next;
    *entry = d;
    return true;
}

static bool server_build_hid(void)
{
    static const uint8_t information[] = {0x11, 0x01, 0x00, 0x02};
    static const uint8_t zero_keyboard[8] = {0};
    static const uint8_t zero_mouse[5] = {0};
    static const uint8_t zero_gamepad[11] = {0};
    static const uint8_t zero = 0;
    const uint32_t secure_read = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC;
    const uint32_t secure_input = secure_read | BLE_GATT_CHR_F_NOTIFY |
        BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC;
    const uint32_t secure_output = secure_read | BLE_GATT_CHR_F_WRITE |
        BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC;
    server_service_t *service = server_add_service_internal("1812");
    if (!service ||
        !server_add_char_internal(service, "2a4a", secure_read,
                                  SERVER_HID_INFORMATION, information,
                                  sizeof(information), true) ||
        !server_add_char_internal(service, "2a4b", secure_read,
                                  SERVER_HID_REPORT_MAP, server_hid_report_map,
                                  sizeof(server_hid_report_map), true) ||
        !server_add_char_internal(service, "2a4c",
                                  BLE_GATT_CHR_F_WRITE_NO_RSP |
                                      BLE_GATT_CHR_F_WRITE_ENC,
                                  SERVER_HID_CONTROL_POINT, &zero, 1, false)) {
        return false;
    }
    server_char_t *keyboard = server_add_char_internal(
        service, "2a4d", secure_input, SERVER_HID_KEYBOARD_INPUT,
        zero_keyboard, sizeof(zero_keyboard), false);
    const uint8_t keyboard_input_ref[] = {1, 1};
    if (!keyboard || !server_add_desc_internal(keyboard, "2908",
                                                BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                                                keyboard_input_ref,
                                                sizeof(keyboard_input_ref))) return false;
    peripheral->hid_keyboard = keyboard->id;

    server_char_t *keyboard_output = server_add_char_internal(
        service, "2a4d", secure_output, SERVER_HID_KEYBOARD_OUTPUT,
        &zero, 1, false);
    const uint8_t keyboard_output_ref[] = {1, 2};
    if (!keyboard_output || !server_add_desc_internal(
            keyboard_output, "2908", BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
            keyboard_output_ref, sizeof(keyboard_output_ref))) return false;

    server_char_t *mouse = server_add_char_internal(
        service, "2a4d", secure_input, SERVER_HID_MOUSE_INPUT,
        zero_mouse, sizeof(zero_mouse), false);
    const uint8_t mouse_ref[] = {2, 1};
    if (!mouse || !server_add_desc_internal(mouse, "2908",
                                             BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                                             mouse_ref, sizeof(mouse_ref))) return false;
    peripheral->hid_mouse = mouse->id;

    server_char_t *gamepad = server_add_char_internal(
        service, "2a4d", secure_input, SERVER_HID_GAMEPAD_INPUT,
        zero_gamepad, sizeof(zero_gamepad), false);
    const uint8_t gamepad_ref[] = {3, 1};
    if (!gamepad || !server_add_desc_internal(gamepad, "2908",
                                               BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
                                               gamepad_ref, sizeof(gamepad_ref))) return false;
    peripheral->hid_gamepad = gamepad->id;
    return true;
}

static esp_err_t server_execute(uint32_t owner, solar_os_ble_server_request_t *r)
{
    if (r->op == SOLAR_OS_BLE_SERVER_CREATE) {
        if (peripheral && peripheral->dormant) {
            const int rc = server_unregister_services();
            if (rc) return solar_os_ble_nimble_error(rc);
            server_free();
        }
        if (peripheral || server_epoch_ticket == UINT32_MAX) return ESP_ERR_INVALID_STATE;
        size_t length = strlen(r->text), capacity = r->capacity ? r->capacity : 16;
        if (!length || length > 26 || capacity > SIZE_MAX / sizeof(solar_os_ble_server_event_t))
            return ESP_ERR_INVALID_ARG;
        server_state_t *s = calloc(1, sizeof(*s));
        if (!s) return ESP_ERR_NO_MEM;
        s->events = calloc(capacity, sizeof(*s->events));
        if (!s->events) { free(s); return ESP_ERR_NO_MEM; }
        s->owner = owner; s->epoch = ++server_epoch_ticket;
        s->kind = SERVER_KIND_GENERIC;
        s->capacity = capacity; memcpy(s->name, r->text, length + 1);
        peripheral = s; return ESP_OK;
    }
    if (!peripheral || peripheral->owner != owner ||
        peripheral->kind != SERVER_KIND_GENERIC) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_STATUS) {
        r->info = (solar_os_ble_server_info_t){.registered=peripheral->registered,
            .advertising=peripheral->advertising, .closing=peripheral->closing,
            .peers=peripheral->peer_count, .services=peripheral->service_count,
            .characteristics=peripheral->char_count, .event_capacity=peripheral->capacity,
            .event_count=peripheral->count, .events_dropped=peripheral->dropped};
        return ESP_OK;
    }
    if (peripheral->closing) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_PEER) {
        server_peer_t *found = NULL;
        for (server_peer_t *p = peripheral->peers; p; p = p->next)
            if (p->id > r->peer && (!found || p->id < found->id)) found = p;
        if (!found) return ESP_ERR_NOT_FOUND;
        r->event = (solar_os_ble_server_event_t){.peer=found->id,
            .mtu=ble_att_mtu(found->conn), .status=found->retiring ? 1 : 0};
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_CLOSE) { peripheral->closing = true; return ESP_OK; }
    if (r->op == SOLAR_OS_BLE_SERVER_POLL) {
        if (!peripheral->count) return ESP_ERR_NOT_FOUND;
        r->event = peripheral->events[peripheral->head];
        peripheral->head = (peripheral->head + 1) % peripheral->capacity; peripheral->count--;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_SERVICE || r->op == SOLAR_OS_BLE_SERVER_CHARACTERISTIC) {
        if (peripheral->registered) return ESP_ERR_INVALID_STATE;
        if (server_local_ticket == UINT32_MAX) return ESP_ERR_NO_MEM;
        ble_uuid_any_t uuid;
        if (!server_uuid(r->text, &uuid)) return ESP_ERR_INVALID_ARG;
        if (r->op == SOLAR_OS_BLE_SERVER_SERVICE) {
            if (peripheral->service_count >= UINT16_MAX) return ESP_ERR_NO_MEM;
            server_service_t **entry = &peripheral->services;
            while (*entry) {
                if (!ble_uuid_cmp(&uuid.u, &(*entry)->uuid.u)) return ESP_ERR_INVALID_ARG;
                entry = &(*entry)->next;
            }
            server_service_t *s = calloc(1, sizeof(*s));
            if (!s) return ESP_ERR_NO_MEM;
            s->uuid = uuid; s->id = ++server_local_ticket; peripheral->service_count++; *entry = s; r->id = s->id;
        } else {
            if (!r->properties || (r->properties & ~0x3e)) return ESP_ERR_INVALID_ARG;
            if (peripheral->char_count >= UINT16_MAX) return ESP_ERR_NO_MEM;
            server_service_t *s = peripheral->services;
            while (s && s->id != r->parent) s = s->next;
            if (!s) return ESP_ERR_INVALID_ARG;
            server_char_t **entry = &s->chars;
            while (*entry) entry = &(*entry)->next;
            server_char_t *c = calloc(1, sizeof(*c));
            if (!c) return ESP_ERR_NO_MEM;
            c->uuid = uuid; c->id = ++server_local_ticket; c->index = peripheral->char_count++;
            c->flags = r->properties;
            c->len = r->value_len; memcpy(c->value, r->value, c->len); *entry = c; r->id = c->id;
        }
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_SERVER_START) {
        int rc = server_register_services();
        if (rc) return solar_os_ble_nimble_error(rc);
        peripheral->wants_advertising = true;
        rc = server_advertise();
        if (rc) peripheral->wants_advertising = false;
        return solar_os_ble_nimble_error(rc);
    }
    if (r->op == SOLAR_OS_BLE_SERVER_STOP) {
        peripheral->wants_advertising = false;
        int rc = peripheral->advertising ? ble_gap_adv_stop() : 0;
        if (!rc || rc == BLE_HS_EALREADY) { peripheral->advertising = false; return ESP_OK; }
        return solar_os_ble_nimble_error(rc);
    }
    server_peer_t *p = server_peer(r->peer, BLE_HS_CONN_HANDLE_NONE);
    if (r->op == SOLAR_OS_BLE_SERVER_DISCONNECT) {
        if (!p || p->retiring) return ESP_ERR_INVALID_STATE;
        int rc = ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
        if (!rc) p->retiring = true;
        return solar_os_ble_nimble_error(rc);
    }
    server_char_t *c = server_characteristic(r->id, 0);
    if (!c) return ESP_ERR_INVALID_ARG;
    if (r->op == SOLAR_OS_BLE_SERVER_SET) {
        memcpy(c->value, r->value, r->value_len); c->len = r->value_len; return ESP_OK;
    }
    if (r->op != SOLAR_OS_BLE_SERVER_SEND || !p || p->retiring) return ESP_ERR_INVALID_STATE;
    if (!(p->subscriptions[c->index] & (r->indicate ? 2 : 1)) || (r->indicate && p->indication_handle))
        return ESP_ERR_INVALID_STATE;
    uint16_t mtu = ble_att_mtu(p->conn);
    if (mtu < 3 || r->value_len > (size_t)(mtu - 3)) return ESP_ERR_INVALID_SIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(r->value, r->value_len);
    if (!om) return ESP_ERR_NO_MEM;
    if (r->indicate) p->indication_handle = c->handle;
    int rc = r->indicate ? ble_gatts_indicate_custom(p->conn, c->handle, om) :
        ble_gatts_notify_custom(p->conn, c->handle, om); /* Both consume om on all outcomes. */
    if (rc && r->indicate) p->indication_handle = 0;
    return solar_os_ble_nimble_error(rc);
}

static server_peer_t *server_hid_peer(void)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID ||
        peripheral->peer_count != 1) return NULL;
    server_peer_t *p = peripheral->peers;
    return p && !p->retiring && p->encrypted ? p : NULL;
}

static bool server_hid_ready(uint32_t characteristic)
{
    server_peer_t *p = server_hid_peer();
    server_char_t *c = server_characteristic(characteristic, 0);
    return p && c && (p->subscriptions[c->index] & 1);
}

static esp_err_t server_hid_send(uint32_t characteristic,
                                 const uint8_t *data,
                                 size_t len)
{
    server_peer_t *p = server_hid_peer();
    server_char_t *c = server_characteristic(characteristic, 0);
    if (!p || !c || !(p->subscriptions[c->index] & 1)) return ESP_ERR_INVALID_STATE;
    const uint16_t mtu = ble_att_mtu(p->conn);
    if (mtu < 3 || len > (size_t)(mtu - 3) || len > sizeof(c->value))
        return ESP_ERR_INVALID_SIZE;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) return ESP_ERR_NO_MEM;
    const int rc = ble_gatts_notify_custom(p->conn, c->handle, om);
    if (!rc) { memcpy(c->value, data, len); c->len = len; }
    return solar_os_ble_nimble_error(rc);
}

static void server_hid_neutralize(void)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID) return;
    const uint8_t keyboard[8] = {0};
    const uint8_t mouse[5] = {0};
    uint8_t gamepad[11] = {0};
    (void)server_hid_send(peripheral->hid_keyboard, keyboard, sizeof(keyboard));
    (void)server_hid_send(peripheral->hid_mouse, mouse, sizeof(mouse));
    (void)server_hid_send(peripheral->hid_gamepad, gamepad, sizeof(gamepad));
    memset(peripheral->keyboard, 0, sizeof(peripheral->keyboard));
    peripheral->mouse_buttons = 0;
    memset(peripheral->gamepad_axes, 0, sizeof(peripheral->gamepad_axes));
    peripheral->gamepad_buttons = 0;
    peripheral->gamepad_hat = 0;
    peripheral->neutralized = true;
}

static bool server_hid_key_apply(uint8_t report[8], uint16_t key, bool pressed)
{
    if (key & SOLAR_OS_HID_KEY_MODIFIER_FLAG) {
        const uint8_t modifier = (uint8_t)key;
        if (!modifier || (modifier & (modifier - 1U))) return false;
        if (pressed) report[0] |= modifier;
        else report[0] &= (uint8_t)~modifier;
        return true;
    }
    if (!key || key > UINT8_MAX) return false;
    for (size_t i = 2; i < 8; i++) {
        if (report[i] != (uint8_t)key) continue;
        if (pressed) return true;
        memmove(&report[i], &report[i + 1], 7 - i);
        report[7] = 0;
        return true;
    }
    if (!pressed) return true;
    for (size_t i = 2; i < 8; i++) {
        if (!report[i]) { report[i] = (uint8_t)key; return true; }
    }
    return false;
}

static int8_t server_hid_axis(int16_t value)
{
    if (value == INT16_MIN) return INT8_MIN + 1;
    if (value == INT16_MAX) return INT8_MAX;
    return (int8_t)(value / 258);
}

static esp_err_t server_hid_execute(uint32_t owner,
                                    solar_os_ble_hid_request_t *r)
{
    if (r->op == SOLAR_OS_BLE_HID_OP_START) {
        const size_t length = strlen(r->name);
        if (!length || length > 26) return ESP_ERR_INVALID_ARG;
        if (peripheral) {
            if (peripheral->kind != SERVER_KIND_HID || peripheral->closing ||
                (!peripheral->dormant &&
                 (peripheral->owner != owner || strcmp(peripheral->name, r->name))))
                return ESP_ERR_INVALID_STATE;
            if (peripheral->dormant) {
                if (server_epoch_ticket == UINT32_MAX) return ESP_ERR_INVALID_ARG;
                peripheral->owner = owner;
                peripheral->epoch = ++server_epoch_ticket;
                peripheral->dormant = false;
                peripheral->neutralized = false;
                memcpy(peripheral->name, r->name, length + 1);
            }
        } else {
            if (server_epoch_ticket == UINT32_MAX) return ESP_ERR_INVALID_ARG;
            server_state_t *s = calloc(1, sizeof(*s));
            if (!s) return ESP_ERR_NO_MEM;
            s->hid_events = calloc(16, sizeof(*s->hid_events));
            if (!s->hid_events) { free(s); return ESP_ERR_NO_MEM; }
            s->owner = owner; s->epoch = ++server_epoch_ticket;
            s->kind = SERVER_KIND_HID; s->capacity = 16;
            memcpy(s->name, r->name, length + 1);
            peripheral = s;
            if (!server_build_hid()) {
                server_free();
                return ESP_ERR_NO_MEM;
            }
        }
        int rc = server_register_services();
        if (rc) return solar_os_ble_nimble_error(rc);
        peripheral->wants_advertising = true;
        rc = server_advertise();
        if (rc) peripheral->wants_advertising = false;
        return solar_os_ble_nimble_error(rc);
    }
    if (!peripheral || peripheral->owner != owner ||
        peripheral->kind != SERVER_KIND_HID) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_HID_OP_STATUS) {
        server_peer_t *p = peripheral->peers;
        r->info = (solar_os_ble_hid_info_t){
            .registered = peripheral->registered,
            .advertising = peripheral->advertising,
            .closing = peripheral->closing,
            .connected = p != NULL && !p->retiring,
            .encrypted = p != NULL && p->encrypted,
            .bonded = p != NULL && p->bonded,
            .keyboard_leds = peripheral->keyboard_leds,
            .event_capacity = peripheral->capacity,
            .event_count = peripheral->count,
            .events_dropped = peripheral->dropped,
        };
        if (p) {
            server_char_t *c = server_characteristic(peripheral->hid_keyboard, 0);
            r->info.keyboard_subscribed = c && (p->subscriptions[c->index] & 1);
            c = server_characteristic(peripheral->hid_mouse, 0);
            r->info.mouse_subscribed = c && (p->subscriptions[c->index] & 1);
            c = server_characteristic(peripheral->hid_gamepad, 0);
            r->info.gamepad_subscribed = c && (p->subscriptions[c->index] & 1);
        }
        return ESP_OK;
    }
    if (peripheral->closing) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_HID_OP_PAIR) {
        peripheral->pairing = true;
        peripheral->hid_bond_valid = false;
        memset(peripheral->hid_subscriptions, 0,
               sizeof(peripheral->hid_subscriptions));
        peripheral->wants_advertising = true;
        server_peer_t *p = peripheral->peers;
        if (!p || p->retiring) return ESP_OK;
        const int rc = ble_gap_terminate(p->conn, BLE_ERR_REM_USER_CONN_TERM);
        if (!rc) p->retiring = true;
        return solar_os_ble_nimble_error(rc);
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_STOP) {
        peripheral->closing = true;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_POLL) {
        if (!peripheral->count) return ESP_ERR_NOT_FOUND;
        r->event = peripheral->hid_events[peripheral->head];
        peripheral->head = (peripheral->head + 1) % peripheral->capacity;
        peripheral->count--;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS ||
        r->op == SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE) {
        if (!r->key_count || r->key_count > 8) return ESP_ERR_INVALID_ARG;
        uint8_t next[8]; memcpy(next, peripheral->keyboard, sizeof(next));
        const bool pressed = r->op == SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS;
        for (size_t i = 0; i < r->key_count; i++)
            if (!server_hid_key_apply(next, r->keys[i], pressed)) return ESP_ERR_INVALID_ARG;
        esp_err_t result = server_hid_send(peripheral->hid_keyboard, next, sizeof(next));
        if (result == ESP_OK) memcpy(peripheral->keyboard, next, sizeof(next));
        return result;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE_ALL) {
        const uint8_t next[8] = {0};
        esp_err_t result = server_hid_send(peripheral->hid_keyboard, next, sizeof(next));
        if (result == ESP_OK) memset(peripheral->keyboard, 0, sizeof(peripheral->keyboard));
        return result;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_MOUSE_BUTTON) {
        const uint8_t valid = SOLAR_OS_HID_MOUSE_LEFT | SOLAR_OS_HID_MOUSE_RIGHT |
            SOLAR_OS_HID_MOUSE_MIDDLE | SOLAR_OS_HID_MOUSE_BACK |
            SOLAR_OS_HID_MOUSE_FORWARD;
        if (!r->button || (r->button & ~valid)) return ESP_ERR_INVALID_ARG;
        uint8_t buttons = peripheral->mouse_buttons;
        if (r->pressed) buttons |= r->button; else buttons &= (uint8_t)~r->button;
        const uint8_t report[5] = {buttons, 0, 0, 0, 0};
        esp_err_t result = server_hid_send(peripheral->hid_mouse, report, sizeof(report));
        if (result == ESP_OK) peripheral->mouse_buttons = buttons;
        return result;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_MOUSE_MOVE) {
        int64_t x = r->x, y = r->y;
        if (x > 32767) x = 32767;
        if (x < -32767) x = -32767;
        if (y > 32767) y = 32767;
        if (y < -32767) y = -32767;
        do {
            const int8_t dx = x > INT8_MAX ? INT8_MAX : x < INT8_MIN ? INT8_MIN : (int8_t)x;
            const int8_t dy = y > INT8_MAX ? INT8_MAX : y < INT8_MIN ? INT8_MIN : (int8_t)y;
            const uint8_t report[5] = {peripheral->mouse_buttons,
                (uint8_t)dx, (uint8_t)dy, 0, 0};
            esp_err_t result = server_hid_send(peripheral->hid_mouse, report, sizeof(report));
            if (result != ESP_OK) return result;
            x -= dx; y -= dy;
        } while (x || y);
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS) {
        if (r->axis < 0 || r->axis >= 6) return ESP_ERR_INVALID_ARG;
        if (!server_hid_ready(peripheral->hid_gamepad)) return ESP_ERR_INVALID_STATE;
        peripheral->gamepad_axes[r->axis] = r->value;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_BUTTON) {
        if (r->button < 1 || r->button > 32) return ESP_ERR_INVALID_ARG;
        if (!server_hid_ready(peripheral->hid_gamepad)) return ESP_ERR_INVALID_STATE;
        const uint32_t mask = 1UL << (r->button - 1U);
        if (r->pressed) peripheral->gamepad_buttons |= mask;
        else peripheral->gamepad_buttons &= ~mask;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_HAT) {
        if (r->hat > SOLAR_OS_HID_HAT_UP_LEFT) return ESP_ERR_INVALID_ARG;
        if (!server_hid_ready(peripheral->hid_gamepad)) return ESP_ERR_INVALID_STATE;
        peripheral->gamepad_hat = r->hat;
        return ESP_OK;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_SEND) {
        uint8_t report[11];
        for (size_t i = 0; i < 6; i++) report[i] = (uint8_t)server_hid_axis(peripheral->gamepad_axes[i]);
        report[6] = peripheral->gamepad_hat;
        for (size_t i = 0; i < 4; i++)
            report[7 + i] = (uint8_t)(peripheral->gamepad_buttons >> (i * 8));
        return server_hid_send(peripheral->hid_gamepad, report, sizeof(report));
    }
    return ESP_ERR_INVALID_ARG;
}

static void server_commands_locked(void)
{
    if (server_command) {
        server_command_t *c = server_command; server_command = NULL;
        c->result = c->abandoned ? SOLAR_OS_BLE_ERR_CANCELLED :
            c->hid ? server_hid_execute(c->owner, &c->request.hid_request) :
                     server_execute(c->owner, &c->request.server_request);
        c->done = true; xSemaphoreGive(c->wake); server_command_release(c);
    }
    server_retire();
    if (peripheral && peripheral->registered) (void)server_advertise();
}
esp_err_t solar_os_ble_backend_server_request(solar_os_ble_session_t owner, solar_os_ble_server_request_t *r)
{
    if (!mutex || !command_ready) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_SERVER_POLL || r->op == SOLAR_OS_BLE_SERVER_STATUS || r->op == SOLAR_OS_BLE_SERVER_SET) {
        /* These operations touch only copied adapter state, never host APIs. */
        lock(); esp_err_t result = server_execute(owner, r); unlock(); return result;
    }
    server_command_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->owner = owner; c->refs = 2; c->request.server_request = *r;
    c->wake = xSemaphoreCreateBinaryStatic(&c->storage);
    lock();
    if (server_command) { c->refs = 1; server_command_release(c); unlock(); return ESP_ERR_INVALID_STATE; }
    server_command = c;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    (void)xSemaphoreTake(c->wake, pdMS_TO_TICKS(5000));
    lock();
    esp_err_t result = c->done ? c->result : ESP_ERR_TIMEOUT;
    if (c->done) *r = c->request.server_request;
    else {
        c->abandoned = true;
        if (peripheral && peripheral->owner == owner) peripheral->closing = true;
    }
    server_command_release(c);
    unlock();
    return result;
}
esp_err_t solar_os_ble_backend_hid_request(solar_os_ble_session_t owner,
                                           solar_os_ble_hid_request_t *r)
{
    if (!mutex || !command_ready) return ESP_ERR_INVALID_STATE;
    if (r->op == SOLAR_OS_BLE_HID_OP_STATUS || r->op == SOLAR_OS_BLE_HID_OP_POLL ||
        r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS ||
        r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_BUTTON ||
        r->op == SOLAR_OS_BLE_HID_OP_GAMEPAD_HAT) {
        lock(); esp_err_t result = server_hid_execute(owner, r); unlock(); return result;
    }
    server_command_t *c = calloc(1, sizeof(*c));
    if (!c) return ESP_ERR_NO_MEM;
    c->owner = owner; c->refs = 2; c->hid = true; c->request.hid_request = *r;
    c->wake = xSemaphoreCreateBinaryStatic(&c->storage);
    lock();
    if (server_command) {
        c->refs = 1; server_command_release(c); unlock();
        return ESP_ERR_INVALID_STATE;
    }
    server_command = c;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    (void)xSemaphoreTake(c->wake, pdMS_TO_TICKS(5000));
    lock();
    esp_err_t result = c->done ? c->result : ESP_ERR_TIMEOUT;
    if (c->done) *r = c->request.hid_request;
    else {
        c->abandoned = true;
        if (peripheral && peripheral->owner == owner) peripheral->closing = true;
    }
    server_command_release(c);
    unlock();
    return result;
}
void solar_os_ble_backend_server_cancel(solar_os_ble_session_t owner)
{
    if (!mutex || !command_ready) return;
    lock();
    if (peripheral && (!owner || peripheral->owner == owner)) peripheral->closing = true;
    if (server_command && (!owner || server_command->owner == owner)) server_command->abandoned = true;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
}
