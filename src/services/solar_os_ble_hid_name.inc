/* Best-effort remote GAP Device Name. Runs asynchronously on the host queue;
 * it neither gates reports nor writes NVS. Callback arguments are monotonic
 * peer IDs, never pointers to connections that teardown can free. */
static server_peer_t *server_name_peer(uint16_t conn, void *arg)
{
    if (!peripheral || peripheral->kind != SERVER_KIND_HID ||
        peripheral->closing || peripheral->dormant) return NULL;
    server_peer_t *p = server_peer((uint32_t)(uintptr_t)arg, conn);
    return p && p->conn == conn && !p->retiring && p->encrypted && p->bonded ? p : NULL;
}

/* Validate UTF-8, retaining a complete prefix when the bounded buffer filled. */
static void server_name_finish(server_peer_t *p, bool truncated)
{
    size_t at = 0;
    while (at < p->name_length) {
        const uint8_t c = (uint8_t)p->name[at];
        size_t width = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 :
            c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!c || !width) goto invalid;
        if (at + width > p->name_length) {
            if (truncated) break;
            goto invalid;
        }
        for (size_t i = 1; i < width; i++)
            if (((uint8_t)p->name[at+i] & 0xc0) != 0x80) goto invalid;
        if ((c == 0xe0 && (uint8_t)p->name[at+1] < 0xa0) ||
            (c == 0xed && (uint8_t)p->name[at+1] >= 0xa0) ||
            (c == 0xf0 && (uint8_t)p->name[at+1] < 0x90) ||
            (c == 0xf4 && (uint8_t)p->name[at+1] >= 0x90)) goto invalid;
        at += width;
    }
    p->name[at] = 0;
    p->name_complete = true;
    return;
invalid:
    p->name[0] = 0;
    p->name_complete = true;
}

static int server_name_value(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    lock();
    server_peer_t *p = server_name_peer(conn, arg);
    if (!p || p->name_complete) { unlock(); return 1; }
    if (error->status == BLE_HS_EDONE) {
        server_name_finish(p, false);
    } else if (error->status || !attr || !attr->om ||
               attr->handle != p->name_handle || attr->offset != p->name_length) {
        p->name[0] = 0;
        p->name_complete = true;
    } else {
        size_t n = OS_MBUF_PKTLEN(attr->om);
        const size_t room = sizeof(p->name) - 1 - p->name_length;
        if (n > room) n = room;
        if (os_mbuf_copydata(attr->om, 0, n, p->name + p->name_length)) {
            p->name[0] = 0;
            p->name_complete = true;
        } else {
            p->name_length += n;
            if (p->name_length == sizeof(p->name) - 1) server_name_finish(p, true);
        }
    }
    const bool done = p->name_complete;
    unlock();
    return done ? 1 : 0;
}

static int server_name_handle(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    lock();
    server_peer_t *p = server_name_peer(conn, arg);
    if (!p || p->name_complete || p->name_handle) { unlock(); return 1; }
    if (error->status || !attr || !attr->handle) {
        p->name_complete = true;
    } else {
        p->name_handle = attr->handle;
        if (ble_gattc_read_long(conn, attr->handle, 0, server_name_value, arg))
            p->name_complete = true;
    }
    unlock();
    return 1; /* One name only; stop the UUID search before starting more reads. */
}

static void server_host_name_request(server_peer_t *p)
{
    if (p->name_requested) return;
    p->name_requested = true;
    const ble_uuid16_t uuid = {.u = {.type = BLE_UUID_TYPE_16}, .value = 0x2a00};
    if (ble_gattc_read_by_uuid(p->conn, 1, UINT16_MAX, &uuid.u,
            server_name_handle, (void *)(uintptr_t)p->id)) p->name_complete = true;
}
