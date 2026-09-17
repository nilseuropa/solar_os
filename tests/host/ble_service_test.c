#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include "freertos/task.h"

#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"

#define TEST_SESSION_COUNT 16
size_t solar_os_ble_backend_capacity(void) { return 1; }
static uint32_t fake_server_owner, fake_server_id, fake_hid_owner;
static solar_os_ble_server_request_t fake_server_request;
static solar_os_ble_hid_request_t fake_hid_request;
static solar_os_ble_hid_device_event_t fake_hid_event;
static bool fake_hid_event_ready;
esp_err_t solar_os_ble_backend_server_request(solar_os_ble_session_t owner, solar_os_ble_server_request_t *r)
{
    if (r->op == SOLAR_OS_BLE_SERVER_CREATE) fake_server_owner = owner;
    if (owner != fake_server_owner) return ESP_ERR_INVALID_STATE;
    fake_server_request = *r;
    if (r->op == SOLAR_OS_BLE_SERVER_SERVICE || r->op == SOLAR_OS_BLE_SERVER_CHARACTERISTIC) r->id = ++fake_server_id;
    if (r->op == SOLAR_OS_BLE_SERVER_STATUS) r->info.event_capacity = 16;
    if (r->op == SOLAR_OS_BLE_SERVER_POLL || r->op == SOLAR_OS_BLE_SERVER_PEER) return ESP_ERR_NOT_FOUND;
    if (r->op == SOLAR_OS_BLE_SERVER_CLOSE) fake_server_owner = 0;
    return ESP_OK;
}
esp_err_t solar_os_ble_backend_hid_request(solar_os_ble_session_t owner,
                                           solar_os_ble_hid_request_t *r)
{
    if (r->op == SOLAR_OS_BLE_HID_OP_START) fake_hid_owner = owner;
    if (owner != fake_hid_owner) return ESP_ERR_INVALID_STATE;
    fake_hid_request = *r;
    if (r->op == SOLAR_OS_BLE_HID_OP_STATUS) r->info.event_capacity = 16;
    if (r->op == SOLAR_OS_BLE_HID_OP_POLL) {
        if (!fake_hid_event_ready) return ESP_ERR_NOT_FOUND;
        r->event = fake_hid_event;
        fake_hid_event_ready = false;
    }
    if (r->op == SOLAR_OS_BLE_HID_OP_STOP) fake_hid_owner = 0;
    return ESP_OK;
}
void solar_os_ble_backend_server_cancel(solar_os_ble_session_t owner)
{
    if (!owner || owner == fake_server_owner) fake_server_owner = 0;
    if (!owner || owner == fake_hid_owner) fake_hid_owner = 0;
}

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_changed = PTHREAD_COND_INITIALIZER;
static solar_os_ble_backend_event_t submitted;
static unsigned submissions, cancellations;
static uint32_t fake_epoch;
static bool initialized, defer_connect, defer_read, defer_write;
static bool defer_subscription;
static bool write_response;
static esp_err_t submit_result;
static const uint8_t peer[6] = {1, 2, 3, 4, 5, 6};

static void record(solar_os_ble_backend_event_t event)
{
    pthread_mutex_lock(&fake_lock);
    submitted = event;
    submissions++;
    pthread_cond_broadcast(&fake_changed);
    pthread_mutex_unlock(&fake_lock);
}

static unsigned submission_count(void)
{
    pthread_mutex_lock(&fake_lock);
    const unsigned n = submissions;
    pthread_mutex_unlock(&fake_lock);
    return n;
}

static solar_os_ble_backend_event_t await_submission(unsigned after)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;
    pthread_mutex_lock(&fake_lock);
    while (submissions == after) {
        assert(pthread_cond_timedwait(&fake_changed, &fake_lock, &deadline) == 0);
    }
    solar_os_ble_backend_event_t event = submitted;
    pthread_mutex_unlock(&fake_lock);
    return event;
}

static void retired(void)
{
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_RETIRED, .epoch = fake_epoch,
    };
    fake_epoch = 0;
    solar_os_ble_service_event(&event);
}

esp_err_t solar_os_ble_backend_register(void) { return ESP_OK; }
void solar_os_ble_backend_reset(void) { fake_epoch = 0; }

esp_err_t solar_os_ble_backend_init(void)
{
    if (!initialized) {
        initialized = solar_os_ble_service_register() == ESP_OK;
    }
    return initialized ? ESP_OK : ESP_FAIL;
}

static size_t scan_count = 1;
static esp_err_t scan_error = ESP_OK;
esp_err_t solar_os_ble_backend_scan(solar_os_ble_scan_result_t *results,
                                   size_t max_results, size_t *found)
{
    assert(max_results >= 1);
    if (scan_error != ESP_OK) return scan_error;
    memset(results, 0, sizeof(*results));
    memcpy(results[0].bda, peer, sizeof(peer));
    strcpy(results[0].name, "Sensor");
    results[0].addr_type = SOLAR_OS_BLE_ADDR_RANDOM;
    results[0].rssi = -73;
    results[0].appearance = 961;
    results[0].hid_service = true;
    results[0].remembered = true;
    *found = scan_count;
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout_ms)
{
    assert(timeout_ms == 1500);
    solar_os_ble_service_reset("sleep");
    solar_os_ble_backend_reset();
    initialized = false;
    return ESP_OK;
}

bool solar_os_ble_backend_sleep_prepare_ready(void) { return true; }
void solar_os_ble_backend_resume(void) { assert(solar_os_ble_backend_init() == ESP_OK); }

esp_err_t solar_os_ble_backend_connect(uint32_t epoch, uint32_t request,
    const uint8_t bda[6], uint8_t addr_type)
{
    assert(epoch != 0 && request != 0);
    if (fake_epoch) return ESP_ERR_INVALID_STATE;
    assert(memcmp(bda, peer, sizeof(peer)) == 0 && addr_type == SOLAR_OS_BLE_ADDR_RANDOM);
    if (submit_result != ESP_OK) {
        return submit_result;
    }
    fake_epoch = epoch;
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_OPENED, .epoch = epoch, .request = request,
        .conn_id = 7, .mtu = 23,
    };
    memcpy(event.bda, peer, sizeof(peer));
    record(event);
    if (defer_connect) {
        return ESP_OK;
    }
    solar_os_ble_service_event(&event);
    event.type = SOLAR_OS_BLE_BACKEND_SERVICE;
    event.service.start_handle = 1;
    event.service.end_handle = 9;
    event.service.primary = true;
    strcpy(event.service.uuid, "0x180f");
    for (unsigned i = 0; i < SOLAR_OS_BLE_GATT_MAX_SERVICES + 1; i++) {
        solar_os_ble_service_event(&event);
    }
    event.type = SOLAR_OS_BLE_BACKEND_MTU;
    event.mtu = 247;
    solar_os_ble_service_event(&event);
    event.type = SOLAR_OS_BLE_BACKEND_DISCOVERED;
    solar_os_ble_service_event(&event);
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_cancel(uint32_t epoch)
{
    assert(epoch == fake_epoch && epoch != 0);
    cancellations++;
    /* Logical cancellation is immediate; transport retirement is controlled
     * separately by the test, like an asynchronous stack unregister event. */
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_characteristics(uint32_t epoch,
    const solar_os_ble_gatt_service_t *service,
    solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    assert(epoch == fake_epoch && service->start_handle == 1);
    if (max_characteristics != 0) {
        characteristics[0].handle = 3;
        characteristics[0].properties = SOLAR_OS_BLE_CHAR_READ;
        strcpy(characteristics[0].uuid, "0x2a19");
        *count = 1;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_read(uint32_t epoch, uint32_t request, uint16_t handle)
{
    assert(epoch == fake_epoch && handle == 3);
    if (submit_result != ESP_OK) {
        return submit_result;
    }
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX + 1];
    memset(value, 0x42, sizeof(value));
    solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_READ, .epoch = epoch, .request = request,
        .conn_id = 7, .handle = handle,
    };
    record(event);
    if (!defer_read) {
        event.value = value;
        event.value_len = sizeof(value);
        solar_os_ble_service_event(&event);
        memset(value, 0xee, sizeof(value)); /* service must copy before returning */
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_write(uint32_t epoch, uint32_t request, uint16_t handle,
    const uint8_t *value, size_t value_len, bool with_response)
{
    assert(epoch == fake_epoch && handle == 3 && value_len == 2);
    assert(value[0] == 0 && value[1] == 0xff);
    write_response = with_response;
    const solar_os_ble_backend_event_t event = {
        .type = SOLAR_OS_BLE_BACKEND_WRITTEN, .epoch = epoch, .request = request,
        .conn_id = 7, .handle = handle,
    };
    record(event);
    if (!defer_write) {
        solar_os_ble_service_event(&event);
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_subscribe(uint32_t epoch, uint32_t request, uint16_t handle, uint8_t mode)
{
    solar_os_ble_backend_event_t e = {.type=SOLAR_OS_BLE_BACKEND_SUBSCRIBED,
        .epoch=epoch,.request=request,.conn_id=7,.handle=handle,.subscription_mode=mode};
    record(e);
    if (!defer_subscription) solar_os_ble_service_event(&e);
    return ESP_OK;
}

typedef struct {
    solar_os_ble_session_t id;
    bool connect;
    bool write;
    esp_err_t result;
    uint8_t value[2];
    size_t len;
} call_t;

static void *run_call(void *arg)
{
    call_t *call = arg;
    if (call->write) {
        const uint8_t value[] = {0, 0xff};
        call->result = solar_os_ble_session_write(call->id, 3, value, sizeof(value), true, 2000);
        return NULL;
    }
    call->result = call->connect ?
        solar_os_ble_session_connect(call->id, peer, SOLAR_OS_BLE_ADDR_RANDOM, 2000) :
        solar_os_ble_session_read(call->id, 3, call->value, sizeof(call->value), &call->len, 2000);
    return NULL;
}

static void connect_session(solar_os_ble_session_t id)
{
    assert(solar_os_ble_session_connect(id, peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);
}

static void assert_busy(solar_os_ble_session_t id)
{
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(id, &info) == ESP_OK && info.busy);
}

typedef struct {
    solar_os_ble_session_t id;
    atomic_bool cancelled;
    atomic_uint checks;
} cancel_context_t;

static bool should_cancel(void *user)
{
    cancel_context_t *ctx = user;
    /* Checks run on the waiting task, without the service metadata lock. */
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(ctx->id, &info) == ESP_OK);
    atomic_fetch_add(&ctx->checks, 1);
    return atomic_load(&ctx->cancelled);
}

static void test_cooperative_cancel(void)
{
    for (int op = 0; op < 3; op++) {
        cancel_context_t ctx = {0};
        assert(solar_os_ble_session_create("script", &ctx.id) == ESP_OK);
        assert(solar_os_ble_session_set_cancel_check(ctx.id, should_cancel, &ctx) == ESP_OK);
        defer_connect = false;
        if (op != 0) {
            connect_session(ctx.id);
        }
        defer_connect = op == 0;
        defer_read = op == 1;
        defer_write = op == 2;
        unsigned n = submission_count();
        call_t call = {.id = ctx.id, .connect = op == 0, .write = op == 2};
        pthread_t thread;
        assert(pthread_create(&thread, NULL, run_call, &call) == 0);
        solar_os_ble_backend_event_t late = await_submission(n);
        assert(solar_os_ble_session_set_cancel_check(ctx.id, NULL, NULL) == ESP_ERR_INVALID_STATE);
        const TickType_t start = xTaskGetTickCount();
        while (atomic_load(&ctx.checks) == 0) {
            const struct timespec pause = {.tv_nsec = 1000000};
            nanosleep(&pause, NULL);
            assert(xTaskGetTickCount() - start < 1000U);
        }
        atomic_store(&ctx.cancelled, true);
        assert(pthread_join(thread, NULL) == 0);
        assert(call.result == SOLAR_OS_BLE_ERR_CANCELLED);
        assert(xTaskGetTickCount() - start < 1000U);
        assert(atomic_load(&ctx.checks) > 0);
        solar_os_ble_service_event(&late);
        solar_os_ble_session_info_t info;
        assert(solar_os_ble_session_get_info(ctx.id, &info) == ESP_OK && info.retiring);
        assert(solar_os_ble_session_close(ctx.id) == ESP_OK);
        assert(solar_os_ble_session_set_cancel_check(ctx.id, NULL, NULL) == ESP_ERR_INVALID_STATE);
        retired();
        /* Reusing the slot must not retain the old callback or its stack context. */
        solar_os_ble_session_t next;
        assert(solar_os_ble_session_create("next", &next) == ESP_OK);
        defer_connect = false;
        connect_session(next);
        assert(solar_os_ble_session_close(next) == ESP_OK);
        retired();
    }
    defer_connect = defer_read = defer_write = false;
}

int main(void)
{
    uint8_t parsed[6];
    assert(solar_os_ble_parse_address("01:02:03:04:05:06", 17, parsed));
    assert(memcmp(parsed, peer, 6) == 0);
    assert(solar_os_ble_parse_address("aA:bB:cC:dD:eE:fF", 17, parsed));
    assert(parsed[0] == 0xaa && parsed[5] == 0xff);
    assert(!solar_os_ble_parse_address("01:02:03:04:05:06x", 18, parsed));
    assert(!solar_os_ble_parse_address("01-02:03:04:05:06", 17, parsed));
    assert(!solar_os_ble_parse_address("01:02:03:04:05:0g", 17, parsed));
    assert(!solar_os_ble_parse_address("01:02:03:04:05:\0X", 17, parsed));
    assert(!solar_os_ble_parse_address(NULL, 17, parsed));
    solar_os_ble_session_t ids[TEST_SESSION_COUNT], extra;
    assert(solar_os_ble_session_create("", &extra) == ESP_ERR_INVALID_ARG);
    for (unsigned i = 0; i < TEST_SESSION_COUNT; i++) {
        assert(solar_os_ble_session_create("test", &ids[i]) == ESP_OK);
    }
    assert(solar_os_ble_session_create("dynamic", &extra) == ESP_OK);
    assert(solar_os_ble_session_close(extra) == ESP_OK);
    const solar_os_ble_session_t stale = ids[3];
    assert(solar_os_ble_session_close(stale) == ESP_OK);
    assert(solar_os_ble_session_create("replacement", &ids[3]) == ESP_OK && ids[3] != stale);
    assert(solar_os_ble_session_cancel(stale) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_close(stale) == ESP_ERR_INVALID_STATE);

    submit_result = ESP_ERR_NO_MEM;
    assert(solar_os_ble_session_connect(ids[0], peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_ERR_NO_MEM);
    submit_result = ESP_OK;
    connect_session(ids[0]);
    solar_os_ble_session_info_t info;
    assert(solar_os_ble_session_get_info(ids[0], &info) == ESP_OK);
    assert(info.gatt.connected && info.gatt.mtu == 247 && info.gatt.conn_id == 7);
    assert(info.gatt.service_count == SOLAR_OS_BLE_GATT_MAX_SERVICES);
    solar_os_ble_scan_result_t scan;
    size_t found = 123;
    assert(solar_os_ble_scan(&scan, 1, &found) == ESP_ERR_INVALID_STATE && found == 0);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    uint8_t value[2];
    size_t count = 0;
    assert(solar_os_ble_session_read(ids[1], 3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    const unsigned before = cancellations;
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK && cancellations == before);
    assert(solar_os_ble_gatt_read(3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK && cancellations == before);

    solar_os_ble_gatt_service_t service;
    assert(solar_os_ble_session_services(ids[0], &service, 1, &count) == ESP_OK);
    assert(count == SOLAR_OS_BLE_GATT_MAX_SERVICES && strcmp(service.uuid, "0x180f") == 0);
    solar_os_ble_gatt_characteristic_t characteristic;
    assert(solar_os_ble_session_characteristics(ids[0], 0, &characteristic, 1, &count) == ESP_OK);
    assert(characteristic.handle == 3);
    assert(solar_os_ble_session_read(ids[0], 3, value, sizeof(value), &count, 100) == ESP_OK);
    assert(count == SOLAR_OS_BLE_GATT_VALUE_MAX && value[0] == 0x42);
    value[0] = 0; value[1] = 0xff;
    assert(solar_os_ble_session_write(ids[0], 3, value, 2, false, 100) == ESP_OK && !write_response);
    assert(solar_os_ble_session_write(ids[0], 3, value, 2, true, 100) == ESP_OK && write_response);

    /* Close wakes an in-flight reader. Its result slot cannot belong to a new owner. */
    defer_read = true;
    unsigned n = submission_count();
    call_t call = {.id = ids[0]};
    pthread_t thread;
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t old_read = await_submission(n);
    assert_busy(ids[0]);
    assert(solar_os_ble_session_read(ids[0], 3, value, sizeof(value), &count, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_close(ids[0]) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_get_info(ids[0], &info) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    retired();
    connect_session(ids[1]); /* Same backend conn_id and characteristic handles. */

    n = submission_count();
    call = (call_t){.id = ids[1]};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t current = await_submission(n);
    old_read.value = (const uint8_t *)"old";
    old_read.value_len = 3;
    solar_os_ble_service_event(&old_read);
    old_read.type = SOLAR_OS_BLE_BACKEND_CLOSED;
    solar_os_ble_service_event(&old_read);
    assert_busy(ids[1]);
    solar_os_ble_backend_event_t wrong = current;
    wrong.request--;
    solar_os_ble_service_event(&wrong);
    wrong = current; wrong.handle++;
    solar_os_ble_service_event(&wrong);
    wrong = current; wrong.conn_id++;
    solar_os_ble_service_event(&wrong);
    assert_busy(ids[1]);
    current.value = (const uint8_t *)"OK";
    current.value_len = 2;
    solar_os_ble_service_event(&current);
    assert(pthread_join(thread, NULL) == 0 && call.result == ESP_OK);
    assert(call.len == 2 && memcmp(call.value, "OK", 2) == 0);

    /* A timeout quarantines its connection until retirement, not just its waiter. */
    assert(solar_os_ble_session_read(ids[1], 3, value, 2, &count, 1) == ESP_ERR_TIMEOUT);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && info.retiring);
    assert(solar_os_ble_session_read(ids[1], 3, value, 2, &count, 1) == ESP_ERR_INVALID_STATE);
    retired();

    /* Cancel while open is pending; a late successful OPEN cannot resurrect it. */
    defer_connect = true;
    n = submission_count();
    call = (call_t){.id = ids[1], .connect = true};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    solar_os_ble_backend_event_t old_open = await_submission(n);
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK);
    solar_os_ble_service_event(&old_open);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && !info.gatt.connected);
    retired();
    defer_connect = false;
    connect_session(ids[1]);

    /* Sleep cancels a reader, preserves its session, and invalidates old epochs. */
    n = submission_count();
    call = (call_t){.id = ids[1]};
    assert(pthread_create(&thread, NULL, run_call, &call) == 0);
    old_read = await_submission(n);
    assert(solar_os_ble_prepare_sleep(1500) == ESP_OK);
    assert(pthread_join(thread, NULL) == 0 && call.result == SOLAR_OS_BLE_ERR_CANCELLED);
    assert(solar_os_ble_session_connect(ids[1], peer, SOLAR_OS_BLE_ADDR_RANDOM, 1) == ESP_ERR_INVALID_STATE);
    assert(solar_os_ble_sleep_prepare_ready());
    solar_os_ble_resume();
    connect_session(ids[1]);
    solar_os_ble_service_event(&old_read);
    solar_os_ble_service_event(&old_open);
    assert(solar_os_ble_session_get_info(ids[1], &info) == ESP_OK && info.gatt.connected);
    assert(solar_os_ble_session_cancel(ids[1]) == ESP_OK);
    retired();
    for (unsigned i = 1; i < TEST_SESSION_COUNT; i++) {
        assert(solar_os_ble_session_close(ids[i]) == ESP_OK);
    }

    /* The reserved compatibility client still works when app slots are released. */
    defer_read = false;
    assert(solar_os_ble_gatt_connect(peer, SOLAR_OS_BLE_ADDR_RANDOM, 100) == ESP_OK);
    assert(solar_os_ble_gatt_read(3, value, 2, &count, 100) == ESP_OK);
    assert(solar_os_ble_gatt_disconnect() == ESP_OK);
    retired();
    assert(solar_os_ble_scan(&scan, 1, &found) == ESP_OK && found == 1);
    test_cooperative_cancel();
    puts("BLE sessions: ownership, cancellation, timeout, stale events, sleep and compatibility OK");
    return 0;
}
