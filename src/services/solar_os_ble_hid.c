#include "solar_os_ble_hid.h"
#include "solar_os_ble_hid_report_map.h"
#include "solar_os_ble_nimble.h"
#include "solar_os_task.h"

#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"

#define HID_SERVICE_MAX 5
#define HID_CHAR_MAX 64
#define HID_REPORT_MAX 32
#define HID_MAP_MAX 2048
#define HID_EVENT_MAX 32
#define HID_SETUP_TIMEOUT_MS 10000

typedef struct {
    uint16_t handle, ccc;
    uint8_t map, id;
    bool battery;
} hid_report_t;
typedef struct {
    solar_os_ble_hid_event_type_t type;
    solar_os_ble_hid_event_t event;
} hid_event_t;
typedef enum { READ_MAP, READ_REFERENCE, WRITE_CCC, READ_BATTERY } phase_t;

/* All discovery and link state belongs to the NimBLE host task. Other tasks
 * submit commands, never lend stack buffers to the host. A slot stays pinned
 * through disconnect AND delivery of its final policy event. */
static solar_os_ble_hid_device_t device;
static struct {
    uint32_t epoch;
    atomic_bool active;
    bool closing, open_sent, connecting, discovery_started;
    bool caller_waiting, release_requested, close_requested;
    bool keepalive_requested, keepalive_in_progress, keepalive_attempted;
    uint32_t keepalive_attempts;
    esp_err_t keepalive_last_status;
    size_t service_count, service_index, char_count, char_index, report_count;
    uint16_t reference, ccc, map_handle, protocol_handle;
    uint16_t keepalive_control_handles[HID_SERVICE_MAX], keepalive_read_handle;
    size_t keepalive_control_count;
    size_t map_len;
    uint8_t map_index;
    bool initial_battery_known;
    uint8_t initial_battery_level;
    phase_t phase;
} hid;
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
static EXT_RAM_BSS_ATTR struct {
#else
static struct {
#endif
    struct ble_gatt_svc services[HID_SERVICE_MAX];
    struct ble_gatt_chr chars[HID_CHAR_MAX];
    hid_report_t reports[HID_REPORT_MAX];
    uint8_t map[HID_MAP_MAX];
    bool keyboard_ids[256];
} hid_data;
static uint32_t next_epoch;
static bool accepting_opens;
static portMUX_TYPE command_lock = portMUX_INITIALIZER_UNLOCKED;
static struct ble_npl_event command_event;
static struct ble_npl_callout deadline;
static TickType_t deadline_tick;
static bool deadline_armed;
static QueueHandle_t events;
static SemaphoreHandle_t open_done;
static TaskHandle_t event_task;
static solar_os_ble_hid_callback_t policy_callback;

static void command(struct ble_npl_event *event);
static void fail(int status);
static void next_characteristic(void);
static void next_service(void);
static int value_callback(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg);
static int keepalive_callback(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg);

static solar_os_ble_hid_keepalive_method_t keepalive_method(void)
{
    if (hid.keepalive_control_count) return SOLAR_OS_BLE_HID_KEEPALIVE_EXIT_SUSPEND;
    if (hid.keepalive_read_handle) return SOLAR_OS_BLE_HID_KEEPALIVE_INFORMATION_READ;
    return SOLAR_OS_BLE_HID_KEEPALIVE_UNAVAILABLE;
}

static void *token(void) { return (void *)(uintptr_t)hid.epoch; }
static bool live(uint16_t conn, void *arg)
{
    return hid.active && !hid.closing && hid.epoch == (uint32_t)(uintptr_t)arg &&
        device.conn_id == conn;
}
static void stop_deadline(void)
{
    deadline_armed = false;
    ble_npl_callout_stop(&deadline);
}
static bool arm(uint32_t ms)
{
    deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ms);
    deadline_armed = true;
    if (ble_npl_callout_reset(&deadline, ble_npl_time_ms_to_ticks32(ms))) {
        fail(BLE_HS_ENOMEM);
        return false;
    }
    return true;
}
static void submit_command(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &command_event);
}
static void release(void)
{
    portENTER_CRITICAL(&command_lock);
    hid.release_requested = true;
    portEXIT_CRITICAL(&command_lock);
    submit_command();
}

static void event_worker(void *unused)
{
    hid_event_t e;
    for (;;) {
        xQueueReceive(events, &e, portMAX_DELAY);
        if (policy_callback) policy_callback(e.type, &e.event);
        if (e.type == SOLAR_OS_BLE_HID_CLOSE) release();
    }
}

static void post_control(solar_os_ble_hid_event_type_t type, int status)
{
    hid_event_t e = {.type = type};
    if (type == SOLAR_OS_BLE_HID_OPEN) {
        e.event.open.dev = &device;
        e.event.open.status = solar_os_ble_nimble_error(status);
    } else {
        e.event.close.dev = &device;
        e.event.close.status = ESP_OK;
        e.event.close.reason = status;
    }
    /* Input events reserve two slots. There can be only one OPEN and CLOSE
     * until CLOSE is consumed; neither lifecycle event can be dropped. */
    configASSERT(xQueueSend(events, &e, 0) == pdTRUE);
}

static void post_battery(uint8_t level)
{
    hid_event_t e = {
        .type = SOLAR_OS_BLE_HID_BATTERY,
        .event.battery.level = level,
    };
    /* Battery telemetry is optional. Keep the reserved lifecycle/input slots
     * available instead of failing an otherwise usable keyboard connection. */
    if (uxQueueSpacesAvailable(events) > 2) {
        (void)xQueueSend(events, &e, 0);
    }
}

static void disconnected(int status)
{
    stop_deadline();
    portENTER_CRITICAL(&command_lock);
    hid.keepalive_requested = false;
    if (hid.keepalive_in_progress) hid.keepalive_last_status = ESP_ERR_INVALID_STATE;
    hid.keepalive_in_progress = false;
    portEXIT_CRITICAL(&command_lock);
    device.connected = false;
    device.conn_id = -1;
    device.status = status ? status : BLE_HS_ENOTCONN;
    hid.closing = true;
    if (hid.open_sent) post_control(SOLAR_OS_BLE_HID_CLOSE, status);
    else {
        xSemaphoreGive(open_done);
        release();
    }
}

static void fail(int status)
{
    if (hid.closing) return;
    device.status = status;
    hid.closing = true;
    stop_deadline();
    if (device.conn_id >= 0) {
        int rc = ble_gap_terminate(device.conn_id, BLE_ERR_REM_USER_CONN_TERM);
        if (rc == BLE_HS_ENOTCONN) disconnected(status);
    } else if (hid.connecting) {
        int rc = ble_gap_conn_cancel();
        if (rc == BLE_HS_EALREADY) disconnected(status);
    } else disconnected(status);
}

static void timeout(struct ble_npl_event *event)
{
    /* Stopping/rearming an NPL timer does not retract an already queued event.
     * Validate the CURRENT deadline so an old event cannot kill a ready link
     * or the next connection occupying this slot. */
    if (hid.active && !hid.closing && deadline_armed &&
        (int32_t)(xTaskGetTickCount() - deadline_tick) >= 0) fail(BLE_HS_ETIMEOUT);
}

static void ready(void)
{
    if (!hid.report_count) { fail(BLE_HS_ENOENT); return; }
    bool keyboard = false;
    for (size_t i = 0; i < hid.report_count; ++i) keyboard |= !hid_data.reports[i].battery;
    if (!keyboard) { fail(BLE_HS_ENOENT); return; }
    stop_deadline();
    device.reports_len = hid.report_count;
    device.connected = true;
    device.status = 0;
    hid.open_sent = true;
    post_control(SOLAR_OS_BLE_HID_OPEN, 0);
    if (hid.initial_battery_known) {
        post_battery(hid.initial_battery_level);
    }
    xSemaphoreGive(open_done);
}

static void subscribe(uint8_t id)
{
    if (!hid.ccc || hid.report_count == HID_REPORT_MAX) { fail(BLE_HS_ENOMEM); return; }
    struct ble_gatt_chr *chr = &hid_data.chars[hid.char_index];
    hid_report_t *r = &hid_data.reports[hid.report_count++];
    r->handle = chr->val_handle;
    r->ccc = hid.ccc;
    r->id = id;
    r->map = hid.map_index;
    r->battery = ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x180f;
    uint8_t value[2] = {(chr->properties & BLE_GATT_CHR_PROP_NOTIFY) ? 1 : 2, 0};
    hid.phase = WRITE_CCC;
    int rc = ble_gattc_write_flat(device.conn_id, r->ccc, value, sizeof(value), value_callback, token());
    if (rc) fail(rc);
}

static void read_battery_or_advance(void)
{
    struct ble_gatt_chr *chr = &hid_data.chars[hid.char_index];
    if (chr->properties & BLE_GATT_CHR_PROP_READ) {
        hid.phase = READ_BATTERY;
        if (!arm(HID_SETUP_TIMEOUT_MS)) return;
        const int rc = ble_gattc_read(device.conn_id, chr->val_handle,
                                      value_callback, token());
        if (!rc) return;
    }
    ++hid.char_index;
    next_characteristic();
}

static int descriptors_callback(uint16_t conn, const struct ble_gatt_error *error,
    uint16_t handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (!live(conn, arg)) return 0;
    if (!error->status) {
        uint16_t uuid = ble_uuid_u16(&dsc->uuid.u);
        if (uuid == 0x2902) hid.ccc = dsc->handle;
        if (uuid == 0x2908) hid.reference = dsc->handle;
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail(error->status); return 0; }
    if (ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x180f) {
        if (hid.ccc) subscribe(0);
        else read_battery_or_advance();
    }
    else if (hid.reference) {
        hid.phase = READ_REFERENCE;
        int rc = ble_gattc_read(conn, hid.reference, value_callback, token());
        if (rc) fail(rc);
    } else {
        ++hid.char_index;
        next_characteristic();
    }
    return 0;
}

static void next_characteristic(void)
{
    bool battery = ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x180f;
    while (hid.char_index < hid.char_count) {
        struct ble_gatt_chr *chr = &hid_data.chars[hid.char_index];
        uint16_t uuid = ble_uuid_u16(&chr->uuid.u);
        const bool subscribable =
            (chr->properties &
             (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) != 0;
        const bool readable_battery =
            battery && (chr->properties & BLE_GATT_CHR_PROP_READ) != 0;
        if (uuid == (battery ? 0x2a19 : 0x2a4d) &&
            (subscribable || readable_battery)) {
            if (!subscribable) {
                read_battery_or_advance();
                return;
            }
            uint16_t end = hid.char_index + 1 < hid.char_count ?
                hid_data.chars[hid.char_index + 1].def_handle - 1 : hid_data.services[hid.service_index].end_handle;
            hid.reference = hid.ccc = 0;
            if (!arm(HID_SETUP_TIMEOUT_MS)) return;
            int rc = ble_gattc_disc_all_dscs(device.conn_id, chr->val_handle, end,
                                            descriptors_callback, token());
            if (rc) fail(rc);
            return;
        }
        ++hid.char_index;
    }
    if (!battery) ++hid.map_index;
    ++hid.service_index;
    next_service();
}

static void map_complete(void)
{
    memset(hid_data.keyboard_ids, 0, sizeof(hid_data.keyboard_ids));
    if (!solar_os_ble_hid_report_map(hid_data.map, hid.map_len, hid_data.keyboard_ids)) {
        fail(BLE_HS_EINVAL);
        return;
    }
    next_characteristic();
}

static int value_callback(uint16_t conn, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    if (!live(conn, arg)) return 0;
    int rc = error->status;
    if (hid.phase == READ_MAP) {
        if (!rc) {
            size_t n = attr && attr->om ? OS_MBUF_PKTLEN(attr->om) : 0;
            if (n > sizeof(hid_data.map) - hid.map_len) { fail(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
            if (n && os_mbuf_copydata(attr->om, 0, n, hid_data.map + hid.map_len)) {
                fail(BLE_HS_EINVAL); return BLE_HS_EINVAL;
            }
            hid.map_len += n;
        } else if (rc == BLE_HS_EDONE) map_complete();
        else fail(rc);
        return 0;
    }
    if (hid.phase == READ_BATTERY) {
        uint8_t level = 0;
        if (!rc && attr && attr->om && OS_MBUF_PKTLEN(attr->om) == 1 &&
            os_mbuf_copydata(attr->om, 0, 1, &level) == 0 && level <= 100U) {
            hid.initial_battery_known = true;
            hid.initial_battery_level = level;
        }
        ++hid.char_index;
        next_characteristic();
        return 0;
    }
    if (rc) { fail(rc); return 0; }
    if (hid.phase == READ_REFERENCE) {
        uint8_t ref[2];
        if (!attr || !attr->om || OS_MBUF_PKTLEN(attr->om) != 2 ||
            os_mbuf_copydata(attr->om, 0, 2, ref)) { fail(BLE_HS_EINVAL); return 0; }
        if (ref[1] == ESP_HID_REPORT_TYPE_INPUT && hid_data.keyboard_ids[ref[0]]) subscribe(ref[0]);
        else { ++hid.char_index; next_characteristic(); }
    } else if (hid.phase == WRITE_CCC) {
        const bool battery =
            ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x180f;
        if (battery) {
            read_battery_or_advance();
            return 0;
        }
        ++hid.char_index;
        next_characteristic();
    }
    return 0;
}

static int keepalive_callback(uint16_t conn, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)error;
    (void)attr;
    if (hid.active && hid.epoch == (uint32_t)(uintptr_t)arg &&
        device.conn_id == conn) {
        portENTER_CRITICAL(&command_lock);
        hid.keepalive_last_status = solar_os_ble_nimble_error(error->status);
        hid.keepalive_in_progress = false;
        portEXIT_CRITICAL(&command_lock);
    }
    return 0;
}

static int characteristics_callback(uint16_t conn, const struct ble_gatt_error *error,
    const struct ble_gatt_chr *chr, void *arg)
{
    if (!live(conn, arg)) return 0;
    if (!error->status) {
        if (hid.char_count == HID_CHAR_MAX) { fail(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
        hid_data.chars[hid.char_count++] = *chr;
        if (ble_uuid_u16(&chr->uuid.u) == 0x2a4b) hid.map_handle = chr->val_handle;
        if (ble_uuid_u16(&chr->uuid.u) == 0x2a4e) hid.protocol_handle = chr->val_handle;
        if (ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x1812 &&
            ble_uuid_u16(&chr->uuid.u) == 0x2a4a &&
            (chr->properties & BLE_GATT_CHR_PROP_READ)) {
            portENTER_CRITICAL(&command_lock);
            hid.keepalive_read_handle = chr->val_handle;
            portEXIT_CRITICAL(&command_lock);
        }
        if (ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x1812 &&
            ble_uuid_u16(&chr->uuid.u) == 0x2a4c &&
            (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
            portENTER_CRITICAL(&command_lock);
            if (hid.keepalive_control_count < HID_SERVICE_MAX) {
                hid.keepalive_control_handles[hid.keepalive_control_count++] =
                    chr->val_handle;
            }
            portEXIT_CRITICAL(&command_lock);
        }
        return 0;
    }
    if (error->status != BLE_HS_EDONE) { fail(error->status); return 0; }
    if (ble_uuid_u16(&hid_data.services[hid.service_index].uuid.u) == 0x180f) {
        next_characteristic();
        return 0;
    }
    if (!hid.map_handle) { fail(BLE_HS_ENOENT); return 0; }
    if (hid.protocol_handle) {
        uint8_t report_mode = 1;
        int rc = ble_gattc_write_no_rsp_flat(conn, hid.protocol_handle, &report_mode, 1);
        if (rc) { fail(rc); return 0; }
    }
    hid.phase = READ_MAP;
    if (!arm(HID_SETUP_TIMEOUT_MS)) return 0;
    int rc = ble_gattc_read_long(conn, hid.map_handle, 0, value_callback, token());
    if (rc) fail(rc);
    return 0;
}

static void next_service(void)
{
    if (hid.service_index == hid.service_count) { ready(); return; }
    hid.char_count = hid.char_index = hid.map_len = 0;
    hid.map_handle = hid.protocol_handle = 0;
    struct ble_gatt_svc *s = &hid_data.services[hid.service_index];
    if (!arm(HID_SETUP_TIMEOUT_MS)) return;
    int rc = ble_gattc_disc_all_chrs(device.conn_id, s->start_handle, s->end_handle,
                                    characteristics_callback, token());
    if (rc) fail(rc);
}

static int services_callback(uint16_t conn, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *svc, void *arg)
{
    if (!live(conn, arg)) return 0;
    if (!error->status) {
        uint16_t uuid = ble_uuid_u16(&svc->uuid.u);
        if (uuid == 0x1812 || uuid == 0x180f) {
            if (hid.service_count == HID_SERVICE_MAX) { fail(BLE_HS_ENOMEM); return BLE_HS_ENOMEM; }
            hid_data.services[hid.service_count++] = *svc;
        }
    } else if (error->status == BLE_HS_EDONE) next_service();
    else fail(error->status);
    return 0;
}

static int mtu_callback(uint16_t conn, const struct ble_gatt_error *error,
                        uint16_t mtu, void *arg)
{
    if (!live(conn, arg)) return 0;
    if (!arm(HID_SETUP_TIMEOUT_MS)) return 0;
    int rc = ble_gattc_disc_all_svcs(conn, services_callback, token());
    if (rc) fail(rc);
    return 0;
}

static int gap_callback(struct ble_gap_event *event, void *arg)
{
    if (!hid.active || hid.epoch != (uint32_t)(uintptr_t)arg) return 0;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (!hid.connecting) break;
        hid.connecting = false;
        if (event->connect.status) { disconnected(event->connect.status); break; }
        device.conn_id = event->connect.conn_handle;
        if (hid.closing) {
            (void)ble_gap_terminate(device.conn_id, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        if (!arm(60000)) break; /* Allow the user to type the pairing passkey. */
        {
            int rc = ble_gap_security_initiate(device.conn_id);
            if (rc && rc != BLE_HS_EALREADY) fail(rc);
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (hid.closing || hid.discovery_started) break;
        if (event->enc_change.status) { fail(event->enc_change.status); break; }
        {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(device.conn_id, &desc);
            if (rc) { fail(rc); break; }
            /* Persist the bonded identity, not a temporary private address.
             * Forget must address the same peer that NimBLE stores in NVS. */
            solar_os_ble_nimble_display_address(device.bda, &desc.peer_id_addr);
            device.addr_type = desc.peer_id_addr.type & 1;
        }
        hid.discovery_started = true;
        if (!arm(HID_SETUP_TIMEOUT_MS)) break;
        {
            int rc = ble_gattc_exchange_mtu(device.conn_id, mtu_callback, token());
            if (rc == BLE_HS_EALREADY) {
                struct ble_gatt_error complete = {.status = 0};
                (void)mtu_callback(device.conn_id, &complete, ble_att_mtu(device.conn_id), token());
            } else if (rc) fail(rc);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (device.conn_id != event->disconnect.conn.conn_handle) break;
        disconnected(event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX:
        if (!device.connected || hid.closing || device.conn_id != event->notify_rx.conn_handle) break;
        for (size_t i = 0; i < hid.report_count; ++i) {
            hid_report_t *r = &hid_data.reports[i];
            if (r->handle != event->notify_rx.attr_handle) continue;
            hid_event_t e = {0};
            size_t n = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (r->battery) {
                if (n != 1) break;
                e.type = SOLAR_OS_BLE_HID_BATTERY;
                os_mbuf_copydata(event->notify_rx.om, 0, 1, &e.event.battery.level);
                if (e.event.battery.level > 100U) break;
            } else {
                if (!n || n > sizeof(e.event.input.data)) { fail(BLE_HS_EINVAL); break; }
                e.type = SOLAR_OS_BLE_HID_INPUT;
                e.event.input.dev = &device;
                e.event.input.usage = ESP_HID_USAGE_KEYBOARD;
                e.event.input.map_index = r->map;
                e.event.input.report_id = r->id;
                e.event.input.length = n;
                os_mbuf_copydata(event->notify_rx.om, 0, n, e.event.input.data);
            }
            if (uxQueueSpacesAvailable(events) <= 2 || xQueueSend(events, &e, 0) != pdTRUE)
                fail(BLE_HS_ENOMEM); /* Disconnect releases any pressed keys. */
            break;
        }
        break;
    default: return solar_os_ble_nimble_security(event);
    }
    return 0;
}

static void command(struct ble_npl_event *event)
{
    portENTER_CRITICAL(&command_lock);
    bool closing = hid.close_requested;
    bool releasing = hid.release_requested && !hid.caller_waiting;
    bool keepalive = hid.keepalive_requested;
    uint16_t keepalive_controls[HID_SERVICE_MAX];
    size_t keepalive_control_count = hid.keepalive_control_count;
    memcpy(keepalive_controls, hid.keepalive_control_handles,
           keepalive_control_count * sizeof(keepalive_controls[0]));
    uint16_t keepalive_read = hid.keepalive_read_handle;
    hid.close_requested = false;
    hid.keepalive_requested = false;
    if (releasing) {
        hid.active = false;
        hid.release_requested = false;
    }
    portEXIT_CRITICAL(&command_lock);
    if (releasing || !hid.active) return;
    if (closing) { fail(BLE_HS_EAPP); return; }
    if (keepalive && device.connected && !hid.closing &&
        (keepalive_control_count || keepalive_read)) {
        portENTER_CRITICAL(&command_lock);
        hid.keepalive_attempted = true;
        ++hid.keepalive_attempts;
        hid.keepalive_last_status = ESP_ERR_NOT_FINISHED;
        hid.keepalive_in_progress = keepalive_control_count == 0;
        portEXIT_CRITICAL(&command_lock);
        int rc;
        if (keepalive_control_count) {
            const uint8_t exit_suspend = 1U;
            esp_err_t result = ESP_OK;
            for (size_t i = 0; i < keepalive_control_count; ++i) {
                rc = ble_gattc_write_no_rsp_flat(device.conn_id,
                                                  keepalive_controls[i],
                                                  &exit_suspend,
                                                  sizeof(exit_suspend));
                if (rc && result == ESP_OK) result = solar_os_ble_nimble_error(rc);
            }
            portENTER_CRITICAL(&command_lock);
            hid.keepalive_last_status = result;
            hid.keepalive_in_progress = false;
            portEXIT_CRITICAL(&command_lock);
        } else {
            rc = ble_gattc_read(device.conn_id, keepalive_read,
                                keepalive_callback, token());
            if (rc) {
                portENTER_CRITICAL(&command_lock);
                hid.keepalive_last_status = solar_os_ble_nimble_error(rc);
                hid.keepalive_in_progress = false;
                portEXIT_CRITICAL(&command_lock);
            }
        }
    }
    if (device.conn_id >= 0 || hid.closing || hid.connecting) return;
    ble_addr_t addr;
    solar_os_ble_nimble_address(&addr, device.bda, device.addr_type);
    if (!arm(4000)) return;
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 3000, NULL, gap_callback, token());
    hid.connecting = rc == 0;
    if (rc) disconnected(rc);
}

esp_err_t solar_os_ble_hid_init(solar_os_ble_hid_callback_t callback)
{
    if (!events) events = xQueueCreate(HID_EVENT_MAX, sizeof(hid_event_t));
    if (!open_done) open_done = xSemaphoreCreateBinary();
    if (!events || !open_done) return ESP_ERR_NO_MEM;
    policy_callback = callback;
    if (!event_task && solar_os_task_create_pinned_internal(event_worker, "ble_hid_events", 4096,
        NULL, 4, &event_task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) return ESP_ERR_NO_MEM;
    ble_npl_event_init(&command_event, command, NULL);
    if (ble_npl_callout_init(&deadline, nimble_port_get_dflt_eventq(), timeout, NULL)) {
        ble_npl_event_deinit(&command_event);
        return ESP_ERR_NO_MEM;
    }
    device.conn_id = -1;
    accepting_opens = true;
    return ESP_OK;
}

bool solar_os_ble_hid_idle(void)
{
    portENTER_CRITICAL(&command_lock);
    bool idle = !hid.active;
    portEXIT_CRITICAL(&command_lock);
    return idle;
}

esp_err_t solar_os_ble_hid_deinit(void)
{
    if (!solar_os_ble_hid_idle()) return ESP_ERR_INVALID_STATE;
    stop_deadline();
    ble_npl_callout_deinit(&deadline);
    ble_npl_event_deinit(&command_event);
    return ESP_OK;
}

solar_os_ble_hid_device_t *solar_os_ble_hid_open(const uint8_t bda[6], uint8_t type)
{
    portENTER_CRITICAL(&command_lock);
    if (!accepting_opens || hid.active || next_epoch == UINT32_MAX) { portEXIT_CRITICAL(&command_lock); return NULL; }
    memset(&hid, 0, sizeof(hid));
    memset(&device, 0, sizeof(device));
    device.conn_id = -1;
    memcpy(device.bda, bda, sizeof(device.bda));
    device.addr_type = type;
    hid.epoch = ++next_epoch;
    hid.active = hid.caller_waiting = true;
    portEXIT_CRITICAL(&command_lock);
    while (xSemaphoreTake(open_done, 0) == pdTRUE) {}
    submit_command();
    /* The host's per-phase deadlines and cancellation run independently. A
     * final outer deadline also bounds a peer with many slow characteristics. */
    bool done = xSemaphoreTake(open_done, pdMS_TO_TICKS(90000)) == pdTRUE;
    if (!done) solar_os_ble_hid_cancel_open();
    solar_os_ble_hid_device_t *result = done && device.connected ? &device : NULL;
    portENTER_CRITICAL(&command_lock);
    hid.caller_waiting = false;
    bool pending_release = hid.release_requested;
    portEXIT_CRITICAL(&command_lock);
    if (pending_release) submit_command();
    return result;
}

esp_err_t solar_os_ble_hid_close(solar_os_ble_hid_device_t *dev)
{
    if (dev != &device) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&command_lock);
    if (!hid.active) { portEXIT_CRITICAL(&command_lock); return ESP_OK; }
    hid.close_requested = true;
    portEXIT_CRITICAL(&command_lock);
    submit_command();
    return ESP_OK;
}

esp_err_t solar_os_ble_hid_keepalive(solar_os_ble_hid_device_t *dev)
{
    if (dev != &device) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&command_lock);
    if (!hid.active || hid.closing || !device.connected) {
        portEXIT_CRITICAL(&command_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (keepalive_method() == SOLAR_OS_BLE_HID_KEEPALIVE_UNAVAILABLE) {
        portEXIT_CRITICAL(&command_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (hid.keepalive_requested || hid.keepalive_in_progress) {
        portEXIT_CRITICAL(&command_lock);
        return ESP_ERR_NOT_FINISHED;
    }
    hid.keepalive_requested = true;
    portEXIT_CRITICAL(&command_lock);
    submit_command();
    return ESP_OK;
}

void solar_os_ble_hid_get_keepalive_status(solar_os_ble_hid_keepalive_status_t *status)
{
    if (status == NULL) return;
    portENTER_CRITICAL(&command_lock);
    *status = (solar_os_ble_hid_keepalive_status_t){
        .method = keepalive_method(),
        .attempts = hid.keepalive_attempts,
        .last_status = hid.keepalive_last_status,
        .attempted = hid.keepalive_attempted,
        .pending = hid.keepalive_requested || hid.keepalive_in_progress,
    };
    portEXIT_CRITICAL(&command_lock);
}

void solar_os_ble_hid_cancel_open(void)
{
    if (!device.connected) (void)solar_os_ble_hid_close(&device);
}

void solar_os_ble_hid_suspend(void)
{
    portENTER_CRITICAL(&command_lock);
    accepting_opens = false;
    portEXIT_CRITICAL(&command_lock);
    (void)solar_os_ble_hid_close(&device);
}

const uint8_t *solar_os_ble_hid_address(solar_os_ble_hid_device_t *dev)
{
    return dev == &device ? device.bda : NULL;
}
