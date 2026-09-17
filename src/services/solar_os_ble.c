#include "solar_os_ble.h"
#include "solar_os_ble_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define BLE_CONNECT_TIMEOUT_MS 12000U
#define BLE_OPERATION_TIMEOUT_MS 5000U

typedef enum { BLE_OP_NONE, BLE_OP_CONNECT, BLE_OP_READ, BLE_OP_WRITE, BLE_OP_SUBSCRIBE } ble_operation_t;

typedef struct ble_session {
    struct ble_session *next;
    solar_os_ble_session_t parent;
    struct {
        uint32_t epoch;
        solar_os_ble_session_t owner;
        bool retiring;
        solar_os_ble_gatt_status_t info;
        solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    } link;
    solar_os_ble_session_t id;
    char owner[SOLAR_OS_BLE_OWNER_MAX];
    bool closing;
    bool busy; /* Pins this slot and its result until the calling task returns. */
    bool pending;
    solar_os_ble_cancel_check_t cancel_check;
    void *cancel_user;
    uint32_t request;
    ble_operation_t op;
    uint16_t handle;
    esp_err_t result;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t value_len;
    StaticSemaphore_t wake_storage;
    SemaphoreHandle_t wake;
    solar_os_ble_notification_t *events;
    size_t event_capacity, event_head, event_count;
    uint32_t events_dropped;
} ble_session_t;

static ble_session_t *sessions;
static solar_os_ble_session_t shell_session;
static StaticSemaphore_t state_storage, dispatch_storage;
static SemaphoreHandle_t state_mutex, dispatch_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t ticket;
static bool online;
static bool sleeping;


esp_err_t solar_os_ble_service_prepare_runtime(void)
{
    portENTER_CRITICAL(&init_lock);
    if (state_mutex == NULL) {
        state_mutex = xSemaphoreCreateMutexStatic(&state_storage);
        dispatch_mutex = xSemaphoreCreateMutexStatic(&dispatch_storage);
    }
    portEXIT_CRITICAL(&init_lock);
    return ESP_OK;
}

static void lock_state(void) { xSemaphoreTake(state_mutex, portMAX_DELAY); }
static void unlock_state(void) { xSemaphoreGive(state_mutex); }
static void lock_dispatch(void)
{
    solar_os_ble_service_prepare_runtime();
    xSemaphoreTake(dispatch_mutex, portMAX_DELAY);
}
static void unlock_dispatch(void) { xSemaphoreGive(dispatch_mutex); }

/* Never wrap: exhaustion fails closed instead of aliasing a historical handle. */
static uint32_t next_ticket_locked(void)
{
    return ticket == UINT32_MAX ? 0 : ++ticket;
}

static ble_session_t *find_locked(solar_os_ble_session_t id)
{
    for (ble_session_t *s = sessions; id && s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

static ble_session_t *live_locked(solar_os_ble_session_t id)
{
    ble_session_t *s = find_locked(id);
    if (!s || s->closing) return NULL;
    if (s->parent) {
        ble_session_t *owner = find_locked(s->parent);
        if (!owner || owner->closing) return NULL;
    }
    return s;
}

static void finish_locked(ble_session_t *s, esp_err_t result)
{
    if (s != NULL && s->busy) {
        s->pending = false;
        s->result = result;
        xSemaphoreGive(s->wake);
    }
}

static void clear_events_locked(ble_session_t *s)
{
    free(s->events);
    s->events = NULL;
    s->event_capacity = s->event_head = s->event_count = 0;
}

static void clear_link_locked(ble_session_t *s, const char *status)
{
    clear_events_locked(s);
    memset(&s->link, 0, sizeof(s->link));
    s->link.info.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
    strlcpy(s->link.info.status, status, sizeof(s->link.info.status));
}


/* A closed entry survives until both its transport and waiting caller retire. */
static void reap_locked(ble_session_t *s)
{
    if (!s->closing || s->busy || s->link.epoch) return;
    ble_session_t **entry = &sessions;
    while (*entry && *entry != s) entry = &(*entry)->next;
    if (*entry) *entry = s->next;
    vSemaphoreDelete(s->wake);
    clear_events_locked(s);
    free(s);
}

static bool any_link_locked(void)
{
    for (ble_session_t *s = sessions; s; s = s->next)
        if (s->link.epoch) return true;
    return false;
}

void solar_os_ble_service_reset(const char *status)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    online = false;
    for (ble_session_t *s = sessions, *next; s; s = next) {
        next = s->next;
        finish_locked(s, SOLAR_OS_BLE_ERR_CANCELLED);
        clear_link_locked(s, status ? status : "idle");
        reap_locked(s);
    }
    unlock_state();
}

esp_err_t solar_os_ble_service_register(void)
{
    const esp_err_t ret = solar_os_ble_backend_register();
    lock_state();
    online = ret == ESP_OK;
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_init(void)
{
    lock_dispatch();
    lock_state();
    const bool blocked = sleeping;
    unlock_state();
    const esp_err_t ret = blocked ? ESP_ERR_INVALID_STATE : solar_os_ble_backend_init();
    unlock_dispatch();
    return ret;
}

esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found)
{
    lock_dispatch();
    lock_state();
    /* The legacy discovery scan blocks for seconds. Do not let it delay
     * cancellation of a live generic client's request. */
    const bool blocked = sleeping || any_link_locked();
    unlock_state();
    if (found != NULL) {
        *found = 0;
    }
    const esp_err_t ret = blocked ? ESP_ERR_INVALID_STATE :
        solar_os_ble_backend_scan(results, max_results, found);
    unlock_dispatch();
    return ret;
}

/* Caller holds dispatch. Wake a waiter before submitting asynchronous teardown. */
static uint32_t retire_locked(solar_os_ble_session_t owner, esp_err_t result)
{
    ble_session_t *s = find_locked(owner);
    finish_locked(s, result);
    if (!s || s->link.epoch == 0 || s->link.owner != owner) {
        return 0;
    }
    s->link.retiring = true;
    clear_events_locked(s);
    s->link.info.connected = false;
    s->link.info.service_count = 0;
    strlcpy(s->link.info.status, "retiring", sizeof(s->link.info.status));
    return s->link.epoch;
}

esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms)
{
    lock_dispatch();
    lock_state();
    sleeping = true;
    /* Iterate by immutable ID: callbacks may free entries while state is unlocked. */
    uint32_t cursor = 0;
    for (;;) {
        ble_session_t *chosen = NULL;
        for (ble_session_t *s = sessions; s; s = s->next)
            if (s->id > cursor && (!chosen || s->id < chosen->id)) chosen = s;
        if (!chosen) break;
        cursor = chosen->id;
        uint32_t epoch = retire_locked(cursor, SOLAR_OS_BLE_ERR_CANCELLED);
        unlock_state();
        if (epoch) (void)solar_os_ble_backend_cancel(epoch);
        lock_state();
    }
    unlock_state();
    solar_os_ble_backend_server_cancel(0);
    const esp_err_t ret = solar_os_ble_backend_prepare_sleep(timeout_ms);
    lock_state();
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED) sleeping = false;
    unlock_state();
    unlock_dispatch();
    return ret;
}

bool solar_os_ble_sleep_prepare_ready(void)
{
    lock_dispatch();
    const bool ready = solar_os_ble_backend_sleep_prepare_ready();
    unlock_dispatch();
    return ready;
}

void solar_os_ble_resume(void)
{
    lock_dispatch();
    lock_state();
    sleeping = false;
    unlock_state();
    solar_os_ble_backend_resume();
    unlock_dispatch();
}

static esp_err_t create_locked(const char *owner, solar_os_ble_session_t parent,
                               solar_os_ble_session_t *id)
{
    ble_session_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->id = next_ticket_locked();
    if (!s->id) { free(s); return ESP_ERR_NO_MEM; }
    s->wake = xSemaphoreCreateBinaryStatic(&s->wake_storage);
    if (!s->wake) { free(s); return ESP_ERR_NO_MEM; }
    s->parent = parent;
    strlcpy(s->owner, owner, sizeof(s->owner));
    clear_link_locked(s, "idle");
    s->next = sessions;
    sessions = s;
    *id = s->id;
    return ESP_OK;
}

esp_err_t solar_os_ble_session_create(const char *owner, solar_os_ble_session_t *session)
{
    if (session != NULL) {
        *session = SOLAR_OS_BLE_SESSION_INVALID;
    }
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= SOLAR_OS_BLE_OWNER_MAX ||
        session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    const esp_err_t ret = create_locked(owner, 0, session);
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_session_set_cancel_check(solar_os_ble_session_t id,
    solar_os_ble_cancel_check_t check, void *user)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->busy) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    for (ble_session_t *child = sessions; child; child = child->next) {
        if (child->parent == id && child->busy) {
            unlock_state(); return ESP_ERR_INVALID_STATE;
        }
    }
    s->cancel_check = check;
    s->cancel_user = user;
    for (ble_session_t *child = sessions; child; child = child->next) {
        if (child->parent == id) {
            child->cancel_check = check;
            child->cancel_user = user;
        }
    }
    unlock_state();
    return ESP_OK;
}

bool solar_os_ble_parse_address(const char *text, size_t len, uint8_t bda[6])
{
    if (text == NULL || bda == NULL || len != 17) {
        return false;
    }
    uint8_t parsed[6] = {0};
    for (size_t i = 0; i < 6; i++) {
        if (i != 0 && text[i * 3 - 1] != ':') {
            return false;
        }
        for (size_t j = 0; j < 2; j++) {
            const char c = text[i * 3 + j];
            const int digit = c >= '0' && c <= '9' ? c - '0' :
                c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0) {
                return false;
            }
            parsed[i] = (uint8_t)((parsed[i] << 4) | digit);
        }
    }
    memcpy(bda, parsed, sizeof(parsed));
    return true;
}

static esp_err_t cancel_session(solar_os_ble_session_t id, bool close)
{
    lock_dispatch();
    lock_state();
    ble_session_t *owner = live_locked(id);
    if (!owner) { unlock_state(); unlock_dispatch(); return ESP_ERR_INVALID_STATE; }
    /* Closing prevents new children before any asynchronous teardown is submitted. */
    if (close) owner->closing = true;
    uint32_t cursor = 0;
    esp_err_t ret = ESP_OK;
    for (;;) {
        ble_session_t *chosen = NULL;
        for (ble_session_t *s = sessions; s; s = s->next)
            if ((s->id == id || s->parent == id) && s->id > cursor &&
                (!chosen || s->id < chosen->id)) chosen = s;
        if (!chosen) break;
        cursor = chosen->id;
        const uint32_t epoch = retire_locked(cursor, SOLAR_OS_BLE_ERR_CANCELLED);
        if (close) chosen->closing = true;
        reap_locked(chosen);
        unlock_state();
        if (epoch) {
            esp_err_t result = solar_os_ble_backend_cancel(epoch);
            if (result != ESP_OK) ret = result;
        }
        lock_state();
    }
    unlock_state();
    solar_os_ble_backend_server_cancel(id);
    unlock_dispatch();
    return ret;
}

esp_err_t solar_os_ble_session_cancel(solar_os_ble_session_t session)
{
    return cancel_session(session, false);
}

esp_err_t solar_os_ble_session_close(solar_os_ble_session_t session)
{
    return cancel_session(session, true);
}

esp_err_t solar_os_ble_server_request(solar_os_ble_session_t session,
                                     solar_os_ble_server_request_t *request)
{
    if (!request || request->op < SOLAR_OS_BLE_SERVER_CREATE || request->op > SOLAR_OS_BLE_SERVER_PEER ||
        request->value_len > SOLAR_OS_BLE_GATT_VALUE_MAX ||
        !memchr(request->text, 0, sizeof(request->text))) return ESP_ERR_INVALID_ARG;
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(session);
    bool valid = s && !s->parent && !sleeping;
    solar_os_ble_cancel_check_t check = valid ? s->cancel_check : NULL;
    void *user = valid ? s->cancel_user : NULL;
    unlock_state();
    esp_err_t result = valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (result == ESP_OK && check && check(user)) result = SOLAR_OS_BLE_ERR_CANCELLED;
    if (result == ESP_OK && request->op == SOLAR_OS_BLE_SERVER_CREATE)
        result = solar_os_ble_backend_init();
    if (result == ESP_OK) result = solar_os_ble_backend_server_request(session, request);
    unlock_dispatch();
    return result;
}

static esp_err_t ble_hid_request(solar_os_ble_session_t session,
                                 solar_os_ble_hid_request_t *request)
{
    if (!request || request->op < SOLAR_OS_BLE_HID_OP_START ||
        request->op > SOLAR_OS_BLE_HID_OP_PAIR ||
        !memchr(request->name, 0, sizeof(request->name))) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(session);
    const bool valid = s && !s->parent && !sleeping;
    solar_os_ble_cancel_check_t check = valid ? s->cancel_check : NULL;
    void *user = valid ? s->cancel_user : NULL;
    unlock_state();
    esp_err_t result = valid ? ESP_OK : ESP_ERR_INVALID_STATE;
    if (result == ESP_OK && check && check(user)) {
        result = SOLAR_OS_BLE_ERR_CANCELLED;
    }
    if (result == ESP_OK && request->op == SOLAR_OS_BLE_HID_OP_START) {
        result = solar_os_ble_backend_init();
    }
    if (result == ESP_OK) {
        result = solar_os_ble_backend_hid_request(session, request);
    }
    unlock_dispatch();
    return result;
}

esp_err_t solar_os_ble_hid_device_start(solar_os_ble_session_t session,
                                        const char *name)
{
    if (!name) return ESP_ERR_INVALID_ARG;
    const size_t length = strlen(name);
    if (!length || length > 26) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_hid_request_t request = {.op = SOLAR_OS_BLE_HID_OP_START};
    memcpy(request.name, name, length + 1);
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_device_stop(solar_os_ble_session_t session)
{
    solar_os_ble_hid_request_t request = {.op = SOLAR_OS_BLE_HID_OP_STOP};
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_device_pair(solar_os_ble_session_t session)
{
    solar_os_ble_hid_request_t request = {.op = SOLAR_OS_BLE_HID_OP_PAIR};
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_device_status(solar_os_ble_session_t session,
                                         solar_os_ble_hid_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    solar_os_ble_hid_request_t request = {.op = SOLAR_OS_BLE_HID_OP_STATUS};
    esp_err_t result = ble_hid_request(session, &request);
    if (result == ESP_OK) *info = request.info;
    return result;
}

esp_err_t solar_os_ble_hid_device_poll(solar_os_ble_session_t session,
                                       solar_os_ble_hid_device_event_t *event)
{
    if (!event) return ESP_ERR_INVALID_ARG;
    solar_os_ble_hid_request_t request = {.op = SOLAR_OS_BLE_HID_OP_POLL};
    esp_err_t result = ble_hid_request(session, &request);
    if (result == ESP_OK) *event = request.event;
    return result;
}

static esp_err_t ble_hid_keyboard(solar_os_ble_session_t session,
                                  solar_os_ble_hid_operation_t op,
                                  const uint16_t *keys,
                                  size_t key_count)
{
    if (!keys || !key_count || key_count > 8) return ESP_ERR_INVALID_ARG;
    solar_os_ble_hid_request_t request = {.op = op, .key_count = key_count};
    memcpy(request.keys, keys, key_count * sizeof(*keys));
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_keyboard_press(solar_os_ble_session_t session,
                                          const uint16_t *keys,
                                          size_t key_count)
{
    return ble_hid_keyboard(session, SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS,
                            keys, key_count);
}

esp_err_t solar_os_ble_hid_keyboard_release(solar_os_ble_session_t session,
                                            const uint16_t *keys,
                                            size_t key_count)
{
    return ble_hid_keyboard(session, SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE,
                            keys, key_count);
}

esp_err_t solar_os_ble_hid_keyboard_release_all(solar_os_ble_session_t session)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE_ALL,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_mouse_move(solar_os_ble_session_t session,
                                      int32_t x,
                                      int32_t y)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_MOUSE_MOVE,
        .x = x,
        .y = y,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_mouse_button(solar_os_ble_session_t session,
                                        uint8_t button,
                                        bool pressed)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_MOUSE_BUTTON,
        .button = button,
        .pressed = pressed,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_gamepad_axis(solar_os_ble_session_t session,
                                        int axis,
                                        int16_t value)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS,
        .axis = axis,
        .value = value,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_gamepad_button(solar_os_ble_session_t session,
                                          uint8_t button,
                                          bool pressed)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_GAMEPAD_BUTTON,
        .button = button,
        .pressed = pressed,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_gamepad_hat(solar_os_ble_session_t session,
                                       uint8_t hat)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_GAMEPAD_HAT,
        .hat = hat,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_hid_gamepad_send(solar_os_ble_session_t session)
{
    solar_os_ble_hid_request_t request = {
        .op = SOLAR_OS_BLE_HID_OP_GAMEPAD_SEND,
    };
    return ble_hid_request(session, &request);
}

esp_err_t solar_os_ble_session_get_info(solar_os_ble_session_t session,
                                      solar_os_ble_session_info_t *info)
{
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = live_locked(session);
    if (s == NULL) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    memset(info, 0, sizeof(*info));
    strlcpy(info->owner, s->owner, sizeof(info->owner));
    info->busy = s->busy;
    info->event_capacity = s->event_capacity;
    info->event_count = s->event_count;
    info->events_dropped = s->events_dropped;
    info->gatt.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
    strlcpy(info->gatt.status, online ? "idle" : "sleep", sizeof(info->gatt.status));
    if (s->link.owner == session) {
        info->retiring = s->link.retiring;
        info->gatt = s->link.info;
    }
    unlock_state();
    return ESP_OK;
}

static esp_err_t begin_locked(ble_session_t *s, ble_operation_t op, uint16_t handle)
{
    if (s == NULL || s->busy || sleeping || !online) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t request = next_ticket_locked();
    if (request == 0) {
        return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s->wake, 0) == pdTRUE) {}
    s->busy = true;
    s->pending = true;
    s->request = request;
    s->op = op;
    s->handle = handle;
    s->result = ESP_OK;
    s->value_len = 0;
    return ESP_OK;
}

static esp_err_t wait_operation(solar_os_ble_session_t id, uint32_t request,
    uint32_t timeout_ms, uint8_t *value, size_t max_len, size_t *value_len)
{
    lock_state();
    ble_session_t *s = find_locked(id); /* busy pins the slot, including after close */
    SemaphoreHandle_t wake = s->wake;
    const solar_os_ble_cancel_check_t check = s->cancel_check;
    void *const user = s->cancel_user;
    unlock_state();
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    esp_err_t aborted = ESP_OK;
    for (;;) {
        if (xSemaphoreTake(wake, 0) == pdTRUE) {
            break;
        }
        if (check != NULL && check(user)) {
            aborted = SOLAR_OS_BLE_ERR_CANCELLED;
            break;
        }
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= timeout) {
            aborted = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t remaining = timeout - elapsed;
        TickType_t slice = pdMS_TO_TICKS(50U);
        if (slice == 0) {
            slice = 1;
        }
        if (check != NULL && remaining > slice) {
            remaining = slice;
        }
        if (xSemaphoreTake(wake, remaining) == pdTRUE) {
            break;
        }
    }
    if (aborted != ESP_OK) {
        lock_dispatch();
        lock_state();
        uint32_t epoch = 0;
        if (s->request == request && s->pending) {
            epoch = retire_locked(id, aborted);
        }
        unlock_state();
        if (epoch != 0) {
            (void)solar_os_ble_backend_cancel(epoch);
        }
        unlock_dispatch();
    }
    lock_state();
    const esp_err_t ret = s->result;
    if (ret == ESP_OK && value_len != NULL) {
        const size_t copied = s->value_len < max_len ? s->value_len : max_len;
        if (copied != 0) {
            memcpy(value, s->value, copied);
        }
        *value_len = s->value_len;
    }
    s->busy = false;
    s->pending = false;
    s->op = BLE_OP_NONE;
    reap_locked(s);
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_session_connect(solar_os_ble_session_t id,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (bda == NULL || addr_type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_dispatch();
    lock_state();
    const bool valid = live_locked(id) != NULL && !sleeping;
    unlock_state();
    esp_err_t ret = valid ? solar_os_ble_backend_init() : ESP_ERR_INVALID_STATE;
    if (ret != ESP_OK) {
        unlock_dispatch();
        return ret;
    }
    lock_state();
    uint32_t cursor = 0;
    for (;;) {
        uint32_t epoch = 0;
        for (ble_session_t *entry = sessions; entry; entry = entry->next)
            if (entry->link.retiring && entry->link.epoch > cursor &&
                (!epoch || entry->link.epoch < epoch)) epoch = entry->link.epoch;
        if (!epoch) break;
        cursor = epoch;
        unlock_state();
        /* Retry orphaned teardown too; never reclaim capacity on elapsed time. */
        (void)solar_os_ble_backend_cancel(epoch);
        lock_state();
    }
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->link.epoch != 0) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    ret = begin_locked(s, BLE_OP_CONNECT, 0);
    if (ret != ESP_OK) {
        unlock_state();
        unlock_dispatch();
        return ret;
    }
    const uint32_t request = s->request;
    /* A connect request is also the unique lifetime token for its transport. */
    clear_link_locked(s, "connecting");
    s->link.epoch = request;
    s->link.owner = id;
    s->link.info.addr_type = addr_type;
    memcpy(s->link.info.bda, bda, sizeof(s->link.info.bda));
    unlock_state();
    ret = solar_os_ble_backend_connect(request, request, bda, addr_type);
    if (ret != ESP_OK) {
        lock_state();
        finish_locked(s, ret);
        clear_link_locked(s, "connect failed");
        unlock_state();
    }
    unlock_dispatch();
    return wait_operation(id, request, timeout_ms != 0 ? timeout_ms : BLE_CONNECT_TIMEOUT_MS,
                          NULL, 0, NULL);
}

esp_err_t solar_os_ble_session_services(solar_os_ble_session_t id,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_services != 0 && services == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->link.owner != id ||
        !s->link.info.connected || s->link.retiring) {
        unlock_state();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t copied = s->link.info.service_count < max_services ?
        s->link.info.service_count : max_services;
    if (copied != 0) {
        memcpy(services, s->link.services, copied * sizeof(*services));
    }
    if (count != NULL) {
        *count = s->link.info.service_count;
    }
    unlock_state();
    return ESP_OK;
}

esp_err_t solar_os_ble_session_characteristics(solar_os_ble_session_t id,
    size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_characteristics != 0 && characteristics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->busy || s->link.owner != id || !s->link.info.connected ||
        s->link.retiring || sleeping) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    if (service_index >= s->link.info.service_count) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_NOT_FOUND;
    }
    const uint32_t epoch = s->link.epoch;
    const solar_os_ble_gatt_service_t service = s->link.services[service_index];
    unlock_state();
    esp_err_t ret = solar_os_ble_backend_characteristics(epoch, &service, characteristics,
                                                         max_characteristics, count);
    lock_state();
    if (s->link.epoch != epoch || s->link.retiring) {
        ret = ESP_ERR_INVALID_STATE;
        if (count != NULL) {
            *count = 0;
        }
    }
    unlock_state();
    unlock_dispatch();
    return ret;
}

static esp_err_t transfer(solar_os_ble_session_t id, uint16_t handle,
    const uint8_t *write_value, size_t write_len, bool with_response,
    uint8_t *read_value, size_t read_max, size_t *read_len, uint32_t timeout_ms, bool write)
{
    lock_dispatch();
    lock_state();
    ble_session_t *s = live_locked(id);
    if (s == NULL || s->link.owner != id || !s->link.info.connected || s->link.retiring) {
        unlock_state();
        unlock_dispatch();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t begin = begin_locked(s, write ? BLE_OP_WRITE : BLE_OP_READ, handle);
    if (begin != ESP_OK) {
        unlock_state();
        unlock_dispatch();
        return begin;
    }
    const uint32_t epoch = s->link.epoch, request = s->request;
    unlock_state();
    const esp_err_t ret = write ?
        solar_os_ble_backend_write(epoch, request, handle, write_value, write_len, with_response) :
        solar_os_ble_backend_read(epoch, request, handle);
    if (ret != ESP_OK) {
        lock_state();
        if (s->pending) {
            finish_locked(s, ret);
        }
        unlock_state();
    }
    unlock_dispatch();
    return wait_operation(id, request, timeout_ms != 0 ? timeout_ms : BLE_OPERATION_TIMEOUT_MS,
                          read_value, read_max, read_len);
}

esp_err_t solar_os_ble_session_read(solar_os_ble_session_t id,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{
    if (value_len != NULL) {
        *value_len = 0;
    }
    if (handle == 0 || (max_len != 0 && value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t ignored;
    return transfer(id, handle, NULL, 0, false, value, max_len,
                    value_len != NULL ? value_len : &ignored, timeout_ms, false);
}

esp_err_t solar_os_ble_session_write(solar_os_ble_session_t id,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{
    if (handle == 0 || value == NULL || value_len == 0 || value_len > SOLAR_OS_BLE_GATT_VALUE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return transfer(id, handle, value, value_len, with_response, NULL, 0, NULL, timeout_ms, true);
}

void solar_os_ble_service_event(const solar_os_ble_backend_event_t *event)
{
    if (event == NULL) {
        return;
    }
    lock_state();
    ble_session_t *s = sessions;
    while (s && (!event->epoch || s->link.epoch != event->epoch)) s = s->next;
    if (!s) { unlock_state(); return; }
    if (event->type == SOLAR_OS_BLE_BACKEND_RETIRED) {
        if (s != NULL && s->pending) {
            finish_locked(s, ESP_FAIL);
        }
        clear_link_locked(s, "disconnected");
        reap_locked(s);
        unlock_state();
        return;
    }
    if (s->link.retiring || s == NULL || s->closing) {
        unlock_state();
        return;
    }
    const bool matched = s->busy && s->pending && s->request == event->request;
    switch (event->type) {
    case SOLAR_OS_BLE_BACKEND_OPENED:
        if (!matched || s->op != BLE_OP_CONNECT) {
            break;
        }
        if (event->result != ESP_OK) {
            s->link.retiring = true;
            finish_locked(s, event->result);
        } else {
            s->link.info.conn_id = event->conn_id;
            s->link.info.mtu = event->mtu;
            strlcpy(s->link.info.status, "discovering", sizeof(s->link.info.status));
        }
        break;
    case SOLAR_OS_BLE_BACKEND_MTU:
        if (event->conn_id == s->link.info.conn_id && event->result == ESP_OK) {
            s->link.info.mtu = event->mtu;
        }
        break;
    case SOLAR_OS_BLE_BACKEND_SERVICE:
        if (matched && s->op == BLE_OP_CONNECT && event->conn_id == s->link.info.conn_id &&
            s->link.info.service_count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            s->link.services[s->link.info.service_count++] = event->service;
        }
        break;
    case SOLAR_OS_BLE_BACKEND_DISCOVERED:
        if (matched && s->op == BLE_OP_CONNECT && event->conn_id == s->link.info.conn_id) {
            s->link.info.connected = event->result == ESP_OK;
            s->link.retiring = event->result != ESP_OK;
            strlcpy(s->link.info.status, event->result == ESP_OK ? "connected" : "discovery failed",
                    sizeof(s->link.info.status));
            finish_locked(s, event->result);
        }
        break;
    case SOLAR_OS_BLE_BACKEND_NOTIFICATION:
        if (s->link.info.connected && event->conn_id == s->link.info.conn_id && s->events) {
            if (event->result != ESP_OK || event->value_len > SOLAR_OS_BLE_GATT_VALUE_MAX ||
                (event->value_len && !event->value) || s->event_count == s->event_capacity) {
                if (s->events_dropped != UINT32_MAX) ++s->events_dropped;
            } else {
                solar_os_ble_notification_t *n =
                    &s->events[(s->event_head + s->event_count++) % s->event_capacity];
                n->handle = event->handle;
                n->indication = event->indication;
                n->value_len = event->value_len;
                if (n->value_len) memcpy(n->value, event->value, n->value_len);
            }
        }
        break;
    case SOLAR_OS_BLE_BACKEND_SUBSCRIBED:
        if (matched && s->op == BLE_OP_SUBSCRIBE && event->conn_id == s->link.info.conn_id &&
            event->handle == s->handle) {
            if (event->result == ESP_OK && !event->subscription_mode) {
                /* Compact in ring order, preserving other characteristics. */
                size_t kept = 0;
                for (size_t i = 0; i < s->event_count; ++i) {
                    solar_os_ble_notification_t *n = &s->events[(s->event_head + i) % s->event_capacity];
                    if (n->handle != event->handle)
                        s->events[(s->event_head + kept++) % s->event_capacity] = *n;
                }
                s->event_count = kept;
            }
            finish_locked(s, event->result);
        }
        break;
    case SOLAR_OS_BLE_BACKEND_READ:
    case SOLAR_OS_BLE_BACKEND_WRITTEN:
        if (matched && event->conn_id == s->link.info.conn_id && s->handle == event->handle &&
            s->op == (event->type == SOLAR_OS_BLE_BACKEND_READ ? BLE_OP_READ : BLE_OP_WRITE)) {
            if (s->op == BLE_OP_READ && event->result == ESP_OK && event->value != NULL) {
                s->value_len = event->value_len < sizeof(s->value) ? event->value_len : sizeof(s->value);
                memcpy(s->value, event->value, s->value_len);
            }
            finish_locked(s, event->result);
        }
        break;
    case SOLAR_OS_BLE_BACKEND_CLOSED:
        if (event->conn_id == s->link.info.conn_id) {
            clear_events_locked(s);
            s->link.retiring = true;
            s->link.info.connected = false;
            s->link.info.service_count = 0;
            strlcpy(s->link.info.status, "disconnected", sizeof(s->link.info.status));
            if (s->pending) {
                finish_locked(s, ESP_FAIL);
            }
        }
        break;
    default:
        break;
    }
    unlock_state();
}

size_t solar_os_ble_peer_capacity(void)
{
    return solar_os_ble_backend_capacity();
}

static bool owns_peer(solar_os_ble_session_t session, solar_os_ble_peer_t peer)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *owner = live_locked(session), *child = live_locked(peer);
    bool valid = owner && !owner->parent && child && child->parent == session;
    unlock_state();
    return valid;
}

static ble_session_t *owned_peer_locked(solar_os_ble_session_t session, solar_os_ble_peer_t peer)
{
    ble_session_t *owner = live_locked(session), *s = live_locked(peer);
    return owner && !owner->parent && s && s->parent == session ? s : NULL;
}

static esp_err_t configure_queue_locked(ble_session_t *s, size_t capacity)
{
    if (!capacity || capacity > SIZE_MAX / sizeof(*s->events)) return ESP_ERR_INVALID_ARG;
    if (!s->link.info.connected || s->link.retiring || s->busy || sleeping || s->event_count)
        return ESP_ERR_INVALID_STATE;
    if (s->event_capacity == capacity) return ESP_OK;
    solar_os_ble_notification_t *events = calloc(capacity, sizeof(*events));
    if (!events) return ESP_ERR_NO_MEM;
    clear_events_locked(s);
    s->events = events;
    s->event_capacity = capacity;
    return ESP_OK;
}

esp_err_t solar_os_ble_peer_configure_queue(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, size_t capacity)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = owned_peer_locked(session, peer);
    esp_err_t ret = s ? configure_queue_locked(s, capacity) : ESP_ERR_INVALID_STATE;
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_peer_poll(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, solar_os_ble_notification_t *event)
{
    if (!event) return ESP_ERR_INVALID_ARG;
    solar_os_ble_service_prepare_runtime();
    lock_state();
    ble_session_t *s = owned_peer_locked(session, peer);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s && s->link.info.connected && !s->link.retiring && !sleeping) {
        ret = ESP_ERR_NOT_FOUND;
        if (s->event_count) {
            *event = s->events[s->event_head];
            s->event_head = (s->event_head + 1) % s->event_capacity;
            --s->event_count;
            ret = ESP_OK;
        }
    }
    unlock_state();
    return ret;
}

esp_err_t solar_os_ble_peer_subscribe(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, uint16_t handle, uint8_t mode, uint32_t timeout_ms)
{
    if (!handle || mode > 2) return ESP_ERR_INVALID_ARG;
    lock_dispatch();
    lock_state();
    ble_session_t *s = owned_peer_locked(session, peer);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s && s->link.info.connected && !s->link.retiring) {
        ret = mode && !s->events ? configure_queue_locked(s, 16) : ESP_OK;
        if (ret == ESP_OK) ret = begin_locked(s, BLE_OP_SUBSCRIBE, handle);
    }
    if (ret != ESP_OK) { unlock_state(); unlock_dispatch(); return ret; }
    const uint32_t epoch = s->link.epoch, request = s->request;
    unlock_state();
    ret = solar_os_ble_backend_subscribe(epoch, request, handle, mode);
    if (ret != ESP_OK) {
        lock_state();
        if (s->pending) finish_locked(s, ret);
        unlock_state();
    }
    unlock_dispatch();
    return wait_operation(peer, request, timeout_ms ? timeout_ms : BLE_OPERATION_TIMEOUT_MS, NULL, 0, NULL);
}

esp_err_t solar_os_ble_peer_connect(solar_os_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms, solar_os_ble_peer_t *peer)
{
    if (!peer) return ESP_ERR_INVALID_ARG;
    *peer = SOLAR_OS_BLE_PEER_INVALID;
    if (!bda || addr_type > SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY) return ESP_ERR_INVALID_ARG;
    lock_dispatch();
    lock_state();
    ble_session_t *owner = live_locked(session);
    solar_os_ble_session_t child = 0;
    esp_err_t ret = owner && !owner->parent && !sleeping ?
        create_locked(owner->owner, session, &child) : ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK) {
        ble_session_t *entry = find_locked(child);
        entry->cancel_check = owner->cancel_check;
        entry->cancel_user = owner->cancel_user;
    }
    unlock_state();
    unlock_dispatch();
    if (ret != ESP_OK) return ret;
    ret = solar_os_ble_session_connect(child, bda, addr_type, timeout_ms);
    if (ret != ESP_OK) {
        (void)solar_os_ble_session_close(child);
        return ret;
    }
    *peer = child;
    return ESP_OK;
}

esp_err_t solar_os_ble_peer_disconnect(solar_os_ble_session_t session, solar_os_ble_peer_t peer)
{
    return owns_peer(session, peer) ? solar_os_ble_session_close(peer) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ble_peer_get_info(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    solar_os_ble_session_info_t *info)
{
    return owns_peer(session, peer) ? solar_os_ble_session_get_info(peer, info) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ble_peer_services(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{
    if (count) *count = 0;
    return owns_peer(session, peer) ?
        solar_os_ble_session_services(peer, services, max_services, count) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ble_peer_characteristics(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    size_t service_index, solar_os_ble_gatt_characteristic_t *chars, size_t max_chars, size_t *count)
{
    if (count) *count = 0;
    return owns_peer(session, peer) ?
        solar_os_ble_session_characteristics(peer, service_index, chars, max_chars, count) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ble_peer_read(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms)
{
    if (value_len) *value_len = 0;
    return owns_peer(session, peer) ?
        solar_os_ble_session_read(peer, handle, value, max_len, value_len, timeout_ms) : ESP_ERR_INVALID_STATE;
}

esp_err_t solar_os_ble_peer_write(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms)
{
    return owns_peer(session, peer) ?
        solar_os_ble_session_write(peer, handle, value, value_len, with_response, timeout_ms) : ESP_ERR_INVALID_STATE;
}

static solar_os_ble_session_t compatibility_session(void)
{
    solar_os_ble_service_prepare_runtime();
    lock_state();
    if (!live_locked(shell_session)) (void)create_locked("ble.shell", 0, &shell_session);
    solar_os_ble_session_t id = shell_session;
    unlock_state();
    return id;
}

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    return solar_os_ble_session_connect(compatibility_session(), bda, addr_type, timeout_ms);
}

esp_err_t solar_os_ble_gatt_disconnect(void)
{
    return solar_os_ble_session_cancel(compatibility_session());
}

void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status)
{
    if (status != NULL) {
        solar_os_ble_session_info_t info = {0};
        if (solar_os_ble_session_get_info(compatibility_session(), &info) != ESP_OK) {
            info.gatt.conn_id = SOLAR_OS_BLE_CONNECTION_INVALID;
        }
        *status = info.gatt;
    }
}

esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count)
{
    return solar_os_ble_session_services(compatibility_session(), services, max_services, count);
}

esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
    solar_os_ble_gatt_characteristic_t *characteristics, size_t max_characteristics, size_t *count)
{
    return solar_os_ble_session_characteristics(compatibility_session(), service_index,
                                               characteristics, max_characteristics, count);
}

esp_err_t solar_os_ble_gatt_read(uint16_t handle, uint8_t *value, size_t max_len,
                               size_t *value_len, uint32_t timeout_ms)
{
    return solar_os_ble_session_read(compatibility_session(), handle, value, max_len, value_len, timeout_ms);
}

esp_err_t solar_os_ble_gatt_write(uint16_t handle, const uint8_t *value, size_t value_len,
                                bool with_response, uint32_t timeout_ms)
{
    return solar_os_ble_session_write(compatibility_session(), handle, value, value_len, with_response, timeout_ms);
}
