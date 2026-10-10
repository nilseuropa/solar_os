/* Host identities only; encryption keys remain in NimBLE's bond store. */
typedef struct {
    ble_addr_t peer;
    ble_addr_t local;
} server_host_record_t;
typedef struct {
    uint8_t version, count;
    server_host_record_t records[SOLAR_OS_BLE_HID_HOST_MAX];
} server_host_store_t;

static bool server_host_bonded(const ble_addr_t *peer)
{
    struct ble_store_key_sec key = {.peer_addr=*peer};
    struct ble_store_value_sec value = {0};
    return !solar_os_ble_nimble_keyboard_bond_remembered(peer) &&
        !ble_store_read_our_sec(&key, &value);
}
static esp_err_t server_hosts_load(server_host_store_t *store)
{
    memset(store, 0, sizeof(*store));
    store->version = 1;
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("hidhosts", NVS_READONLY, &handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc) return rc;
    size_t length = sizeof(*store);
    rc = nvs_get_blob(handle, "hosts", store, &length);
    nvs_close(handle);
    if (rc == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (rc) return rc;
    if (store->version != 1 || store->count > SOLAR_OS_BLE_HID_HOST_MAX ||
        length != offsetof(server_host_store_t, records) +
                      store->count * sizeof(store->records[0]))
        return ESP_ERR_INVALID_SIZE;
    for (size_t i=0; i<store->count; i++) {
        const server_host_record_t *r = &store->records[i];
        if (r->peer.type > 1 || r->local.type > 1 ||
            (r->local.type == BLE_ADDR_RANDOM && (r->local.val[5] & 0xc0) != 0xc0))
            return ESP_ERR_INVALID_ARG;
        for (size_t j=0; j<i; j++)
            if (server_same_address(&r->peer, &store->records[j].peer))
                return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
static esp_err_t server_hosts_save(const server_host_store_t *store)
{
    nvs_handle_t handle;
    esp_err_t rc = nvs_open("hidhosts", NVS_READWRITE, &handle);
    if (rc) return rc;
    rc = nvs_set_blob(handle, "hosts", store,
        offsetof(server_host_store_t, records) + store->count * sizeof(store->records[0]));
    if (!rc) rc = nvs_commit(handle);
    nvs_close(handle);
    return rc;
}
static int server_hosts_import(int type, union ble_store_value *value, void *arg)
{
    (void)type;
    server_host_store_t *store = arg;
    server_char_t *c = server_characteristic(0, value->cccd.chr_val_handle);
    if (!c || server_hid_subscription_slot(c->hid_kind) < 0 ||
        !server_host_bonded(&value->cccd.peer_addr)) return 0;
    for (size_t i=0; i<store->count; i++)
        if (server_same_address(&store->records[i].peer, &value->cccd.peer_addr)) return 0;
    if (store->count == SOLAR_OS_BLE_HID_HOST_MAX) return 0;
    server_host_record_t *record = &store->records[store->count];
    record->peer = value->cccd.peer_addr;
    record->local.type = BLE_ADDR_PUBLIC;
    if (!ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, record->local.val, NULL)) store->count++;
    return 0;
}
static esp_err_t server_hosts_read(server_host_store_t *store)
{
    esp_err_t rc = server_hosts_load(store);
    if (rc) return rc;
    /* Import public-identity hosts created before managed leases. Existing
     * managed identities must never be replaced by the public identity. */
    const size_t before = store->count;
    bool pruned = false;
    for (size_t i=0; i<store->count;) {
        if (!server_host_bonded(&store->records[i].peer)) {
            memmove(&store->records[i], &store->records[i+1],
                (--store->count-i) * sizeof(store->records[0]));
            pruned = true;
        } else i++;
    }
    int import_rc = ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, server_hosts_import, store);
    if (import_rc) return solar_os_ble_nimble_error(import_rc);
    return pruned || store->count != before ? server_hosts_save(store) : ESP_OK;
}
static esp_err_t server_hosts_remember(const ble_addr_t *peer, const ble_addr_t *local)
{
    server_host_store_t store;
    esp_err_t rc = server_hosts_load(&store);
    if (rc) return rc;
    size_t at = 0;
    while (at < store.count && !server_same_address(peer, &store.records[at].peer)) at++;
    if (at == store.count) {
        /* Expired security records do not occupy host-list capacity. */
        for (size_t i=0; i<store.count;)
            if (!server_host_bonded(&store.records[i].peer)) {
                memmove(&store.records[i], &store.records[i+1],
                    (--store.count-i) * sizeof(store.records[0]));
            } else i++;
        at = store.count;
        if (at == SOLAR_OS_BLE_HID_HOST_MAX) return ESP_ERR_NO_MEM;
        store.count++;
    }
    store.records[at] = (server_host_record_t){.peer=*peer,.local=*local};
    return server_hosts_save(&store);
}
