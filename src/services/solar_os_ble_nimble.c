#include "solar_os_ble_nimble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "solar_os_hid.h"

typedef enum { OP_NONE, OP_CONNECT, OP_READ, OP_WRITE, OP_SUBSCRIBE } operation_t;
typedef struct {
    solar_os_ble_gatt_characteristic_t info;
    uint16_t definition, cccd;
    uint8_t mode;
} characteristic_cache_t;
typedef struct {
    uint16_t start, end;
    size_t count;
    characteristic_cache_t *chars;
} service_cache_t;

static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_event command_event;
static bool command_ready;
static void command_callback(struct ble_npl_event *event);
typedef struct ble_client {
    struct ble_client *next;
    bool queued;
    uint32_t epoch, request;
    uint16_t conn, handle;
    uint16_t descriptor_end;
    uint8_t subscription_mode;
    characteristic_cache_t *subscription;
    bool subscription_writing;
    uint8_t bda[6];
    operation_t op;
    bool retiring, connecting, response;
    uint8_t addr_type;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len;
    size_t count, discovering;
    size_t including;
    service_cache_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
} ble_client_t;
static ble_client_t *clients;
static size_t server_used_locked(void);
static bool server_idle_locked(void);
static void server_reset_locked(void);
static void server_commands_locked(void);

static ble_client_t *find_epoch(uint32_t epoch)
{
    for (ble_client_t *c = clients; c; c = c->next)
        if (epoch && c->epoch == epoch) return c;
    return NULL;
}

static ble_client_t *find_request(uint32_t request)
{
    for (ble_client_t *c = clients; c; c = c->next)
        if (request && c->request == request) return c;
    return NULL;
}

size_t solar_os_ble_backend_capacity(void)
{
    size_t capacity = CONFIG_BT_NIMBLE_MAX_CONNECTIONS;
#ifdef CONFIG_BTDM_CTRL_BLE_MAX_CONN
    if (capacity > CONFIG_BTDM_CTRL_BLE_MAX_CONN) capacity = CONFIG_BTDM_CTRL_BLE_MAX_CONN;
#endif
#ifdef CONFIG_BT_CTRL_BLE_MAX_ACT
    /* Keep one controller activity available for scanning. */
    if (capacity >= CONFIG_BT_CTRL_BLE_MAX_ACT) capacity = CONFIG_BT_CTRL_BLE_MAX_ACT - 1;
#endif
    return capacity > 0 ? capacity - 1 : 0; /* Reserve the HID link even while absent. */
}

static void lock(void) { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(mutex); }

esp_err_t solar_os_ble_nimble_error(int status)
{
    switch (status) {
    case 0: return ESP_OK;
    case BLE_HS_ENOMEM: return ESP_ERR_NO_MEM;
    case BLE_HS_EINVAL: return ESP_ERR_INVALID_ARG;
    case BLE_HS_ETIMEOUT: return ESP_ERR_TIMEOUT;
    case BLE_HS_EBUSY:
    case BLE_HS_EALREADY:
    case BLE_HS_ENOTCONN:
    case BLE_HS_ENOTSYNCED: return ESP_ERR_INVALID_STATE;
    default: return ESP_FAIL;
    }
}

void solar_os_ble_nimble_address(ble_addr_t *out, const uint8_t bda[6], uint8_t type)
{
    out->type = type;
    for (size_t i = 0; i < 6; ++i) out->val[i] = bda[5 - i];
}

void solar_os_ble_nimble_display_address(uint8_t out[6], const ble_addr_t *addr)
{
    for (size_t i = 0; i < 6; ++i) out[i] = addr->val[5 - i];
}

static void uuid_string(const ble_uuid_t *uuid, char *out, size_t size)
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        snprintf(out, size, "0x%04x", BLE_UUID16(uuid)->value);
    } else if (uuid->type == BLE_UUID_TYPE_32) {
        snprintf(out, size, "0x%08lx", (unsigned long)BLE_UUID32(uuid)->value);
    } else {
        ble_uuid_to_str(uuid, out);
    }
}

esp_err_t solar_os_ble_backend_register(void)
{
    portENTER_CRITICAL(&init_lock);
    if (!mutex) {
        mutex = xSemaphoreCreateRecursiveMutexStatic(&mutex_storage);
    }
    portEXIT_CRITICAL(&init_lock);
    lock();
    if (!command_ready) {
        ble_npl_event_init(&command_event, command_callback, NULL);
        command_ready = true;
    }
    unlock();
    return ESP_OK;
}

static void clear_locked(ble_client_t *client)
{
    ble_client_t **entry = &clients;
    while (*entry && *entry != client) entry = &(*entry)->next;
    if (*entry) *entry = client->next;
    for (size_t i = 0; i < client->count; ++i) free(client->services[i].chars);
    free(client);
    if (command_ready && !server_idle_locked())
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
}

void solar_os_ble_backend_reset(void)
{
    if (!mutex) return;
    lock();
    while (clients) clear_locked(clients); /* Host is stopped; callbacks drained. */
    server_reset_locked();
    unlock();
}

bool solar_os_ble_nimble_client_idle(void)
{
    if (!mutex) return true;
    lock();
    bool idle = clients == NULL && server_idle_locked();
    unlock();
    return idle;
}

void solar_os_ble_nimble_host_stopped(void)
{
    if (command_ready) {
        ble_npl_event_deinit(&command_event);
        command_ready = false;
    }
}

static bool matches(ble_client_t *client, uint16_t conn, void *arg)
{
    return client && client->epoch && client->epoch == (uint32_t)(uintptr_t)arg &&
        client->conn == conn && !client->retiring;
}

static solar_os_ble_backend_event_t event_locked(ble_client_t *client, solar_os_ble_backend_event_type_t type, int status)
{
    solar_os_ble_backend_event_t e = {
        .type = type, .epoch = client->epoch, .request = client->request,
        .conn_id = client->conn, .handle = client->handle,
        .status = status, .result = solar_os_ble_nimble_error(status),
    };
    memcpy(e.bda, client->bda, sizeof(e.bda));
    return e;
}

static void retire_locked(ble_client_t *client)
{
    client->retiring = true;
    if (client->conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(client->conn, BLE_ERR_REM_USER_CONN_TERM);
    } else if (client->connecting) {
        (void)ble_gap_conn_cancel();
    } else {
        solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_RETIRED, 0);
        clear_locked(client);
        solar_os_ble_service_event(&e);
    }
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{
    lock();
    ble_client_t *client = find_epoch(epoch);
    if (client) { client->retiring = true; client->queued = true; }
    unlock();
    if (client) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return ESP_OK;
}

static int chars_callback(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg);

static int discover_next_locked(ble_client_t *client, void *arg)
{
    service_cache_t *s = &client->services[client->discovering];
    s->chars = calloc(SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS, sizeof(*s->chars));
    if (!s->chars) return BLE_HS_ENOMEM;
    return ble_gattc_disc_all_chrs(client->conn, s->start, s->end, chars_callback, arg);
}

static int included_callback(uint16_t conn, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *svc, void *arg);

static int discover_included_locked(ble_client_t *client, void *arg)
{
    service_cache_t *s = &client->services[client->including];
    return ble_gattc_find_inc_svcs(client->conn, s->start, s->end, included_callback, arg);
}

static int included_callback(uint16_t conn, const struct ble_gatt_error *error,
    const struct ble_gatt_svc *svc, void *arg)
{
    lock();
    ble_client_t *client = find_epoch((uint32_t)(uintptr_t)arg);
    if (!matches(client, conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    if (!rc) {
        for (size_t i = 0; i < client->count; ++i) {
            if (client->services[i].start == svc->start_handle) { unlock(); return 0; }
        }
        if (client->count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            service_cache_t *s = &client->services[client->count++];
            s->start = svc->start_handle; s->end = svc->end_handle;
            solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_SERVICE, 0);
            e.service.start_handle = s->start; e.service.end_handle = s->end;
            e.service.primary = false;
            uuid_string(&svc->uuid.u, e.service.uuid, sizeof(e.service.uuid));
            unlock();
            solar_os_ble_service_event(&e);
            return 0;
        }
        rc = BLE_HS_ENOMEM;
    } else if (rc == BLE_HS_EDONE) {
        rc = ++client->including < client->count ? discover_included_locked(client, arg) : discover_next_locked(client, arg);
        if (!rc) { unlock(); return 0; }
    }
    solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
    retire_locked(client);
    unlock();
    solar_os_ble_service_event(&e);
    return rc;
}

static int chars_callback(uint16_t conn, const struct ble_gatt_error *error,
                          const struct ble_gatt_chr *chr, void *arg)
{
    lock();
    ble_client_t *client = find_epoch((uint32_t)(uintptr_t)arg);
    if (!matches(client, conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    if (!rc) {
        service_cache_t *s = &client->services[client->discovering];
        if (s->count < SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS) {
            characteristic_cache_t *cached = &s->chars[s->count++];
            cached->definition = chr->def_handle;
            solar_os_ble_gatt_characteristic_t *c = &cached->info;
            c->handle = chr->val_handle;
            c->properties = chr->properties;
            uuid_string(&chr->uuid.u, c->uuid, sizeof(c->uuid));
        } else rc = BLE_HS_ENOMEM;
    } else if (rc == BLE_HS_EDONE) {
        service_cache_t *s = &client->services[client->discovering];
        if (!s->count) {
            free(s->chars);
            s->chars = NULL;
        } else {
            void *compact = realloc(s->chars, s->count * sizeof(*s->chars));
            if (compact) s->chars = compact;
        }
        if (++client->discovering < client->count) {
            rc = discover_next_locked(client, arg);
            if (!rc) { unlock(); return 0; }
        } else rc = 0;
        solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        client->op = OP_NONE;
        client->request = 0;
        if (rc) retire_locked(client);
        unlock();
        solar_os_ble_service_event(&e);
        return 0;
    }
    if (rc) {
        solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        retire_locked(client);
        unlock();
        solar_os_ble_service_event(&e);
        return rc;
    }
    unlock();
    return 0;
}

static int services_callback(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    lock();
    ble_client_t *client = find_epoch((uint32_t)(uintptr_t)arg);
    if (!matches(client, conn, arg)) { unlock(); return 0; }
    int rc = error->status;
    solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_SERVICE, 0);
    if (!rc && client->count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
        service_cache_t *s = &client->services[client->count++];
        s->start = svc->start_handle;
        s->end = svc->end_handle;
        e.service.start_handle = s->start;
        e.service.end_handle = s->end;
        e.service.primary = true;
        uuid_string(&svc->uuid.u, e.service.uuid, sizeof(e.service.uuid));
        unlock();
        solar_os_ble_service_event(&e);
        return 0;
    }
    if (rc == BLE_HS_EDONE) {
        rc = client->count ? discover_included_locked(client, arg) : BLE_HS_ENOENT;
        if (!rc) { unlock(); return 0; }
    } else if (!rc) rc = BLE_HS_ENOMEM;
    e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
    retire_locked(client);
    unlock();
    solar_os_ble_service_event(&e);
    return rc;
}

static int mtu_callback(uint16_t conn, const struct ble_gatt_error *error,
                        uint16_t mtu, void *arg)
{
    lock();
    ble_client_t *client = find_epoch((uint32_t)(uintptr_t)arg);
    if (!matches(client, conn, arg)) { unlock(); return 0; }
    solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_MTU, error->status);
    e.mtu = ble_att_mtu(conn);
    unlock();
    solar_os_ble_service_event(&e);
    lock();
    if (!matches(client, conn, arg)) { unlock(); return 0; }
    int rc = ble_gattc_disc_all_svcs(conn, services_callback, arg);
    if (rc) {
        e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
        retire_locked(client);
    }
    unlock();
    if (rc) solar_os_ble_service_event(&e);
    return 0;
}

static characteristic_cache_t *find_characteristic(ble_client_t *client, uint16_t handle, uint16_t *end)
{
    for (size_t i = 0; i < client->count; ++i) {
        service_cache_t *s = &client->services[i];
        for (size_t j = 0; j < s->count; ++j) {
            if (s->chars[j].info.handle == handle) {
                if (end) {
                    *end = s->end;
                    if (j + 1 < s->count) {
                        uint16_t next = s->chars[j + 1].definition;
                        if (next <= handle) *end = handle; /* Malformed discovery cannot widen the range. */
                        else if (next - 1 < *end) *end = next - 1;
                    }
                }
                return &s->chars[j];
            }
        }
    }
    return NULL;
}

static int gap_callback(struct ble_gap_event *event, void *arg)
{
    lock();
    ble_client_t *client = find_epoch((uint32_t)(uintptr_t)arg);
    if (!client) { unlock(); return 0; }
    solar_os_ble_backend_event_t e;
    if (event->type == BLE_GAP_EVENT_NOTIFY_RX) {
        if (client->retiring || client->conn != event->notify_rx.conn_handle) { unlock(); return 0; }
        characteristic_cache_t *c = find_characteristic(client, event->notify_rx.attr_handle, NULL);
        const uint8_t mode = event->notify_rx.indication ? 2 : 1;
        if (!c || c->mode != mode) { unlock(); return 0; }
        uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
        e = event_locked(client, SOLAR_OS_BLE_BACKEND_NOTIFICATION, 0);
        e.handle = event->notify_rx.attr_handle;
        e.indication = event->notify_rx.indication;
        e.value_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (e.value_len > sizeof(value)) e.result = ESP_ERR_INVALID_SIZE;
        else if (os_mbuf_copydata(event->notify_rx.om, 0, e.value_len, value)) e.result = ESP_FAIL;
        else e.value = value;
        unlock();
        solar_os_ble_service_event(&e);
        return 0; /* NimBLE owns ATT indication confirmation, not the app. */
    }
    if (event->type == BLE_GAP_EVENT_CONNECT) {
        client->connecting = false;
        int rc = event->connect.status;
        if (!rc) client->conn = event->connect.conn_handle;
        if (client->retiring && !rc) { retire_locked(client); unlock(); return 0; }
        e = event_locked(client, SOLAR_OS_BLE_BACKEND_OPENED, rc);
        e.mtu = rc ? 23 : ble_att_mtu(client->conn);
        if (rc) {
            uint32_t epoch = client->epoch;
            clear_locked(client);
            unlock();
            solar_os_ble_service_event(&e);
            e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
            e.epoch = epoch;
            solar_os_ble_service_event(&e);
            return 0;
        }
        unlock();
        solar_os_ble_service_event(&e);
        lock();
        if (matches(client, event->connect.conn_handle, arg)) {
            rc = ble_gattc_exchange_mtu(client->conn, mtu_callback, arg);
            if (rc == BLE_HS_EALREADY) {
                struct ble_gatt_error complete = {.status = 0};
                (void)mtu_callback(client->conn, &complete, ble_att_mtu(client->conn), arg);
                rc = 0;
            }
            if (rc) {
                e = event_locked(client, SOLAR_OS_BLE_BACKEND_DISCOVERED, rc);
                retire_locked(client);
            }
        }
        unlock();
        if (rc) solar_os_ble_service_event(&e);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        if (client->conn != event->disconnect.conn.conn_handle) { unlock(); return 0; }
        e = event_locked(client, SOLAR_OS_BLE_BACKEND_CLOSED, 0);
        e.reason = event->disconnect.reason;
        clear_locked(client);
        unlock();
        solar_os_ble_service_event(&e);
        /* NimBLE aborts outstanding ATT procedures before GAP disconnect. All
         * callbacks additionally carry the immutable epoch, never a slot ptr. */
        e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
        solar_os_ble_service_event(&e);
        return 0;
    }
    unlock();
    return solar_os_ble_nimble_security(event);
}

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request,
                                       const uint8_t bda[6], uint8_t type)
{
    lock();
    size_t count = server_used_locked();
    for (ble_client_t *c = clients; c; c = c->next) {
        ++count;
        if (c->epoch == epoch || (c->addr_type == type && !memcmp(c->bda, bda, 6))) {
            unlock(); return ESP_ERR_INVALID_STATE;
        }
    }
    if (count >= solar_os_ble_backend_capacity()) { unlock(); return SOLAR_OS_BLE_ERR_CAPACITY; }
    ble_client_t *client = calloc(1, sizeof(*client));
    if (!client) { unlock(); return ESP_ERR_NO_MEM; }
    client->conn = BLE_HS_CONN_HANDLE_NONE;
    client->epoch = epoch;
    client->request = request;
    client->op = OP_CONNECT;
    client->queued = true;
    memcpy(client->bda, bda, sizeof(client->bda));
    client->addr_type = type;
    client->next = clients;
    clients = client;
    unlock();
    /* Service dispatch pins host lifetime until submission returns. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *chars, size_t max, size_t *count)
{
    lock();
    ble_client_t *client = find_epoch(epoch);
    if (!client || client->retiring || client->op != OP_NONE) {
        unlock(); return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < client->count; ++i) {
        service_cache_t *s = &client->services[i];
        if (s->start == service->start_handle && s->end == service->end_handle) {
            size_t n = s->count < max ? s->count : max;
            for (size_t j = 0; j < n; ++j) chars[j] = s->chars[j].info;
            if (count) *count = n;
            unlock(); return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

static int value_callback(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    lock();
    ble_client_t *client = find_request((uint32_t)(uintptr_t)arg);
    if (!client || client->retiring || client->conn != conn ||
        client->request != (uint32_t)(uintptr_t)arg ||
        (client->op != OP_READ && client->op != OP_WRITE)) {
        unlock(); return 0;
    }
    solar_os_ble_backend_event_t e = event_locked(client, client->op == OP_READ ?
        SOLAR_OS_BLE_BACKEND_READ : SOLAR_OS_BLE_BACKEND_WRITTEN, error->status);
    if (!error->status && client->op == OP_READ && attr && attr->om) {
        size_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > sizeof(value)) n = sizeof(value);
        if (os_mbuf_copydata(attr->om, 0, n, value) == 0) {
            e.value = value; e.value_len = n;
        } else e.result = ESP_FAIL;
    }
    client->op = OP_NONE;
    client->request = 0;
    unlock();
    solar_os_ble_service_event(&e);
    return 0;
}

static bool ready(ble_client_t *client, uint32_t epoch)
{
    return client && epoch && client->epoch == epoch && !client->retiring &&
        client->conn != BLE_HS_CONN_HANDLE_NONE && client->op == OP_NONE;
}

esp_err_t solar_os_ble_backend_subscribe(uint32_t epoch, uint32_t request, uint16_t handle, uint8_t mode)
{
    if (!handle || mode > 2) return ESP_ERR_INVALID_ARG;
    lock();
    ble_client_t *client = find_epoch(epoch);
    if (!ready(client, epoch)) { unlock(); return ESP_ERR_INVALID_STATE; }
    uint16_t end;
    characteristic_cache_t *c = find_characteristic(client, handle, &end);
    if (!c) { unlock(); return ESP_ERR_NOT_FOUND; }
    uint8_t required = mode == 1 ? SOLAR_OS_BLE_CHAR_NOTIFY : SOLAR_OS_BLE_CHAR_INDICATE;
    if (mode && !(c->info.properties & required)) { unlock(); return ESP_ERR_NOT_SUPPORTED; }
    if (end <= handle) { unlock(); return ESP_ERR_NOT_FOUND; }
    client->op = OP_SUBSCRIBE;
    client->request = request;
    client->handle = handle;
    client->descriptor_end = end;
    client->subscription = c;
    client->subscription_mode = mode;
    client->subscription_writing = false;
    client->queued = true;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return ESP_OK;
}

/* Called with adapter lock; emits completion after releasing it. */
static void subscription_complete_locked(ble_client_t *client, int status)
{
    solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_SUBSCRIBED, status);
    if (status == BLE_HS_ENOENT) e.result = ESP_ERR_NOT_FOUND;
    e.subscription_mode = client->subscription_mode;
    if (!status) client->subscription->mode = client->subscription_mode;
    else if (!client->subscription_writing) client->subscription->cccd = 0;
    client->op = OP_NONE;
    client->request = 0;
    client->subscription = NULL;
    unlock();
    solar_os_ble_service_event(&e);
}

static int subscription_write_callback(uint16_t conn, const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr, void *arg)
{
    lock();
    ble_client_t *client = find_request((uint32_t)(uintptr_t)arg);
    if (!client || client->retiring || client->conn != conn || client->op != OP_SUBSCRIBE ||
        !client->subscription_writing || (!error->status && (!attr || attr->handle != client->subscription->cccd))) {
        unlock(); return 0;
    }
    subscription_complete_locked(client, error->status);
    return 0;
}

static int subscription_write_locked(ble_client_t *client)
{
    client->subscription_writing = true;
    const uint8_t value[2] = {client->subscription_mode, 0};
    return ble_gattc_write_flat(client->conn, client->subscription->cccd, value, sizeof(value),
        subscription_write_callback, (void *)(uintptr_t)client->request);
}

static int subscription_descriptors_callback(uint16_t conn, const struct ble_gatt_error *error,
    uint16_t chr_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    lock();
    ble_client_t *client = find_request((uint32_t)(uintptr_t)arg);
    if (!client || client->retiring || client->conn != conn || client->op != OP_SUBSCRIBE ||
        client->subscription_writing) { unlock(); return 0; }
    int rc = error->status;
    if (!rc) {
        if (chr_handle == client->handle && dsc && dsc->handle > client->handle &&
            dsc->handle <= client->descriptor_end && ble_uuid_u16(&dsc->uuid.u) == 0x2902)
            client->subscription->cccd = dsc->handle;
        unlock(); return 0;
    }
    if (rc == BLE_HS_EDONE) {
        rc = client->subscription->cccd ? subscription_write_locked(client) : BLE_HS_ENOENT;
        if (!rc) { unlock(); return 0; }
    }
    subscription_complete_locked(client, rc);
    return 0;
}

esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    lock();
    ble_client_t *client = find_epoch(epoch);
    if (!ready(client, epoch)) { unlock(); return ESP_ERR_INVALID_STATE; }
    client->op = OP_READ; client->request = request; client->handle = handle;
    client->queued = true;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_write(uint32_t epoch, uint32_t request, uint16_t handle,
    const uint8_t *value, size_t len, bool response)
{
    lock();
    ble_client_t *client = find_epoch(epoch);
    if (!ready(client, epoch)) { unlock(); return ESP_ERR_INVALID_STATE; }
    if (len > sizeof(client->value)) { unlock(); return ESP_ERR_INVALID_SIZE; }
    client->op = OP_WRITE; client->request = request; client->handle = handle;
    memcpy(client->value, value, len);
    client->value_len = len;
    client->response = response;
    client->queued = true;
    unlock();
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
    return ESP_OK;
}

/* Submission never acquires NimBLE's host lock from an application task while
 * holding our mutex. All stack calls run on the host queue, avoiding AB/BA
 * deadlocks with a callback arriving during cancel/read/write. */
static void command_client(ble_client_t *client)
{
    if (client->retiring) { retire_locked(client); unlock(); return; }
    int rc = 0;
    void *arg = (void *)(uintptr_t)client->epoch;
    solar_os_ble_backend_event_type_t type;
    if (client->op == OP_CONNECT) {
        if (client->connecting || client->conn != BLE_HS_CONN_HANDLE_NONE) { unlock(); return; }
        ble_addr_t addr;
        solar_os_ble_nimble_address(&addr, client->bda, client->addr_type);
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 3000, NULL, gap_callback, arg);
        client->connecting = rc == 0;
        if (!rc) { unlock(); return; }
        solar_os_ble_backend_event_t e = event_locked(client, SOLAR_OS_BLE_BACKEND_OPENED, rc);
        clear_locked(client);
        unlock();
        solar_os_ble_service_event(&e);
        e.type = SOLAR_OS_BLE_BACKEND_RETIRED;
        solar_os_ble_service_event(&e);
        return;
    } else if (client->op == OP_SUBSCRIBE) {
        rc = client->subscription->cccd ? subscription_write_locked(client) :
            ble_gattc_disc_all_dscs(client->conn, client->handle, client->descriptor_end,
                subscription_descriptors_callback, (void *)(uintptr_t)client->request);
        if (rc) subscription_complete_locked(client, rc);
        else unlock();
        return;
    } else if (client->op == OP_READ) {
        type = SOLAR_OS_BLE_BACKEND_READ;
        rc = ble_gattc_read(client->conn, client->handle, value_callback,
                            (void *)(uintptr_t)client->request);
        if (!rc) { unlock(); return; }
    } else if (client->op == OP_WRITE) {
        type = SOLAR_OS_BLE_BACKEND_WRITTEN;
        if (client->value_len > (size_t)(ble_att_mtu(client->conn) - 3)) rc = BLE_HS_EMSGSIZE;
        else if (client->response) rc = ble_gattc_write_flat(client->conn, client->handle,
            client->value, client->value_len, value_callback, (void *)(uintptr_t)client->request);
        else rc = ble_gattc_write_no_rsp_flat(client->conn, client->handle, client->value, client->value_len);
        if (!rc && client->response) { unlock(); return; }
    } else { unlock(); return; }
    solar_os_ble_backend_event_t e = event_locked(client, type, rc);
    if (rc == BLE_HS_EMSGSIZE) e.result = ESP_ERR_INVALID_SIZE;
    client->op = OP_NONE;
    client->request = 0;
    unlock();
    solar_os_ble_service_event(&e);
}

/* A single coalesced queue event drains only newly queued work, never resubmits
 * an in-flight ATT request when a different peer queues a command. */
#include "solar_os_ble_nimble_server.inc"

static void command_callback(struct ble_npl_event *event)
{
    (void)event;
    for (;;) {
        lock();
        ble_client_t *client = clients;
        while (client && !client->queued) client = client->next;
        if (!client) { server_commands_locked(); unlock(); return; }
        client->queued = false;
        command_client(client); /* Releases mutex; may retire/free this entry. */
    }
}
