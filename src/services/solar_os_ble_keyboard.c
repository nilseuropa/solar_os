#include "solar_os_ble_keyboard.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_hid_common.h"
#include "esp_hidh.h"
#include "esp_hidh_gattc.h"
#include "esp_private/esp_hidh_private.h"
#include "solar_os_log.h"
#include "solar_os_power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BLE_KEYBOARD_SCAN_SECONDS 8
#define BLE_KEYBOARD_NAME_MAX SOLAR_OS_BLE_KEYBOARD_NAME_MAX
#define BLE_KEYBOARD_CHAR_QUEUE_LEN 128
#define BLE_KEYBOARD_MAX_KEYS 6
#define BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS 250
#define BLE_KEYBOARD_RECONNECT_FAST_RETRY_DELAY_MS 250
#define BLE_KEYBOARD_RECONNECT_FAST_WINDOW_MS 5000
#define BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS 1000
#define BLE_KEYBOARD_PAIR_SWITCH_DISCONNECT_TIMEOUT_MS 1200
#define BLE_KEYBOARD_RESUME_RECONNECT_DELAY_MS 100
#define BLE_KEYBOARD_OPEN_TIMEOUT_MS 10000
#define BLE_KEYBOARD_STALE_CLOSE_TIMEOUT_MS 1500
#define BLE_KEYBOARD_PEER_MAGIC 0x4b424431U
#define BLE_KEYBOARD_MAX_REMEMBERED SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED
#define BLE_KEYBOARD_NVS_MIGRATE_MAX_REMEMBERED 3
#define BLE_KEYBOARD_NVS_NAMESPACE "blekbd"
#define BLE_KEYBOARD_NVS_PEERS_KEY "peers"
#define BLE_KEYBOARD_NVS_LEGACY_PEER_KEY "peer"
#define BLE_KEYBOARD_NVS_LAYOUT_KEY "layout"
#define BLE_KEYBOARD_NVS_REPEAT_RATE_KEY "repeat_cps"
#define BLE_KEYBOARD_NVS_REPEAT_DELAY_KEY "repeat_delay"
#define BLE_KEYBOARD_REPEAT_RATE_DEFAULT 15U
#define BLE_KEYBOARD_REPEAT_DELAY_DEFAULT_MS 450U
#define BLE_KEYBOARD_REPEAT_SEQUENCE_MAX 2U
#define BLE_GATT_APP_ID 1U
#define BLE_GATT_CONNECT_TIMEOUT_MS 12000U
#define BLE_GATT_OPERATION_TIMEOUT_MS 5000U
#define BLE_GATT_INVALID_CONN_ID UINT16_MAX
#define HID_MOD_CTRL 0x11
#define HID_MOD_SHIFT 0x22
#define HID_MOD_LEFT_ALT 0x04
#define HID_MOD_RIGHT_ALT 0x40
#define HID_MOD_ALT (HID_MOD_LEFT_ALT | HID_MOD_RIGHT_ALT)
#define LATIN1_A_UMLAUT_UPPER ((char)0xc4)
#define LATIN1_O_UMLAUT_UPPER ((char)0xd6)
#define LATIN1_U_UMLAUT_UPPER ((char)0xdc)
#define LATIN1_SHARP_S ((char)0xdf)
#define LATIN1_A_UMLAUT_LOWER ((char)0xe4)
#define LATIN1_O_UMLAUT_LOWER ((char)0xf6)
#define LATIN1_U_UMLAUT_LOWER ((char)0xfc)

typedef enum {
    BLE_KEYBOARD_IDLE,
    BLE_KEYBOARD_SCANNING,
    BLE_KEYBOARD_CONNECTING,
    BLE_KEYBOARD_CONNECTED,
    BLE_KEYBOARD_PASSKEY,
    BLE_KEYBOARD_PAIRING_PENDING,
    BLE_KEYBOARD_FAILED,
} ble_keyboard_state_t;

typedef enum {
    BLE_KEYBOARD_SCAN_DISCOVERY,
    BLE_KEYBOARD_SCAN_PAIRING,
} ble_keyboard_scan_mode_t;

typedef struct {
    bool valid;
    bool keyboard_like;
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    char name[BLE_KEYBOARD_NAME_MAX];
} ble_keyboard_candidate_t;

typedef struct {
    uint32_t magic;
    esp_bd_addr_t bda;
    uint8_t addr_type;
    char name[BLE_KEYBOARD_NAME_MAX];
} ble_keyboard_peer_t;

typedef struct {
    bool active;
    uint8_t keycode;
    uint8_t sequence_len;
    char sequence[BLE_KEYBOARD_REPEAT_SEQUENCE_MAX];
    uint32_t next_ms;
} ble_keyboard_repeat_state_t;

typedef enum {
    BLE_GATT_OP_NONE,
    BLE_GATT_OP_CONNECT,
    BLE_GATT_OP_READ,
    BLE_GATT_OP_WRITE,
} ble_gatt_operation_t;

typedef struct {
    bool connected;
    bool registered;
    bool connecting;
    esp_gatt_if_t gattc_if;
    uint16_t conn_id;
    uint16_t mtu;
    esp_bd_addr_t bda;
    esp_ble_addr_type_t addr_type;
    solar_os_ble_gatt_service_t services[SOLAR_OS_BLE_GATT_MAX_SERVICES];
    size_t service_count;
    ble_gatt_operation_t op;
    esp_gatt_status_t op_status;
    uint8_t op_value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    size_t op_value_len;
    char status[80];
} ble_gatt_state_t;

static const char *TAG = "ble_keyboard";

static SemaphoreHandle_t scan_done_sem;
static SemaphoreHandle_t close_done_sem;
static SemaphoreHandle_t status_mutex;
static SemaphoreHandle_t gatt_mutex;
static SemaphoreHandle_t gatt_op_sem;
static QueueHandle_t char_queue;
static TaskHandle_t scan_task_handle;
static TaskHandle_t reconnect_task_handle;
static TickType_t reconnect_fast_until_tick;
static portMUX_TYPE repeat_lock = portMUX_INITIALIZER_UNLOCKED;
static bool initialized;
static bool hidh_initialized;
static bool classic_bt_memory_released;
static bool connected;
static bool reconnect_suppressed_for_sleep;
static bool reconnect_suppressed_for_pairing;
static bool pairing_retry_pending;
static bool pairing_cancel_requested;
static ble_keyboard_scan_mode_t active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
static bool caps_lock;
static uint8_t previous_keys[BLE_KEYBOARD_MAX_KEYS];
static uint8_t previous_modifiers;
static esp_hidh_dev_t *connected_dev;
static esp_hidh_dev_t *pending_dev;
static TickType_t pending_open_started_tick;
static ble_keyboard_state_t state = BLE_KEYBOARD_IDLE;
static ble_keyboard_candidate_t candidate;
static solar_os_ble_keyboard_scan_result_t *active_scan_results;
static size_t active_scan_max_results;
static size_t active_scan_result_count;
static ble_keyboard_peer_t remembered_peers[BLE_KEYBOARD_MAX_REMEMBERED];
static solar_os_ble_keyboard_layout_t keyboard_layout = SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
static uint16_t repeat_rate_cps = BLE_KEYBOARD_REPEAT_RATE_DEFAULT;
static uint16_t repeat_delay_ms = BLE_KEYBOARD_REPEAT_DELAY_DEFAULT_MS;
static ble_keyboard_repeat_state_t repeat_state;
static ble_gatt_state_t gatt_state = {
    .gattc_if = ESP_GATT_IF_NONE,
    .conn_id = BLE_GATT_INVALID_CONN_ID,
    .status = "idle",
};
static esp_gatt_if_t hid_gattc_if = ESP_GATT_IF_NONE;
static esp_bd_addr_t pending_bda;
static esp_ble_addr_type_t pending_addr_type;
static char pending_name[BLE_KEYBOARD_NAME_MAX];
static char connected_name[BLE_KEYBOARD_NAME_MAX];
static char status_text[80] = "idle";

static esp_ble_scan_params_t scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = ESP_BLE_GAP_SCAN_ITVL_MS(50),
    .scan_window = ESP_BLE_GAP_SCAN_WIN_MS(30),
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

static const char *keyboard_layout_names[] = {
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_US] = "us",
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE] = "de",
};

static const char *addr_type_name(esp_ble_addr_type_t addr_type);
static void set_status(ble_keyboard_state_t next_state, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void repeat_clear(void);
static void schedule_reconnect(uint32_t delay_ms);
static void repeat_queue_if_due(void);
static bool hidh_link_ready(esp_hidh_dev_t *dev, esp_gap_conn_params_t *params);
static bool drop_existing_hidh_device(const uint8_t *bda, const char *reason, uint32_t timeout_ms);
static void restore_status_after_scan(ble_keyboard_scan_mode_t mode);
static esp_err_t run_keyboard_scan(ble_keyboard_scan_mode_t mode);
static esp_err_t start_pairing_scan_now(void);
static void request_pairing_after_pending_connect(const char *reason);
static void ble_gattc_callback(esp_gattc_cb_event_t event,
                               esp_gatt_if_t gattc_if,
                               esp_ble_gattc_cb_param_t *param);

static void log_conn_params(const char *prefix, const esp_gap_conn_params_t *params)
{
    if (params == NULL) {
        return;
    }

    SOLAR_OS_LOGI(TAG,
             "%s conn interval=%u ms latency=%u timeout=%u ms",
             prefix != NULL ? prefix : "ble",
             (unsigned)params->interval,
             (unsigned)params->latency,
             (unsigned)params->timeout * 10U);
}

static void clear_runtime_connection_state(const char *reason)
{
    connected = false;
    connected_dev = NULL;
    pending_dev = NULL;
    pending_open_started_tick = 0;
    memset(previous_keys, 0, sizeof(previous_keys));
    previous_modifiers = 0;
    repeat_clear();
    if (reason != NULL) {
        set_status(BLE_KEYBOARD_IDLE, "%s", reason);
    }
}

static bool reconnect_fast_active(void)
{
    if (reconnect_fast_until_tick == 0) {
        return false;
    }

    const TickType_t now = xTaskGetTickCount();
    return (int32_t)(reconnect_fast_until_tick - now) > 0;
}

static void start_fast_reconnect_window(const char *reason)
{
    reconnect_fast_until_tick = xTaskGetTickCount() +
        pdMS_TO_TICKS(BLE_KEYBOARD_RECONNECT_FAST_WINDOW_MS);
    SOLAR_OS_LOGI(TAG,
             "%s: fast reconnect window %u ms",
             reason != NULL ? reason : "reconnect",
             (unsigned)BLE_KEYBOARD_RECONNECT_FAST_WINDOW_MS);
}

static bool reconnect_is_suppressed(void)
{
    return reconnect_suppressed_for_sleep ||
        reconnect_suppressed_for_pairing ||
        pairing_retry_pending;
}

static void set_status(ble_keyboard_state_t next_state, const char *fmt, ...)
{
    char buffer[sizeof(status_text)];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (status_mutex != NULL) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }

    state = next_state;
    strlcpy(status_text, buffer, sizeof(status_text));

    if (status_mutex != NULL) {
        xSemaphoreGive(status_mutex);
    }
}

static void stop_reconnect_task(const char *reason)
{
    if (reconnect_task_handle == NULL) {
        return;
    }

    if (state == BLE_KEYBOARD_SCANNING) {
        SOLAR_OS_LOGI(TAG,
                      "%s: stopping reconnect scan",
                      reason != NULL ? reason : "ble");
        (void)esp_ble_gap_stop_scanning();
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        set_status(BLE_KEYBOARD_IDLE, "%s", reason != NULL ? reason : "idle");
    }

    TaskHandle_t task = reconnect_task_handle;
    reconnect_task_handle = NULL;
    vTaskDelete(task);
}

static void stop_scan_task_for_sleep(uint32_t timeout_ms)
{
    if (scan_task_handle == NULL && state != BLE_KEYBOARD_SCANNING) {
        return;
    }

    pairing_cancel_requested = true;
    if (state == BLE_KEYBOARD_SCANNING) {
        SOLAR_OS_LOGI(TAG, "sleep: stopping BLE scan");
        (void)esp_ble_gap_stop_scanning();
    }
    if (scan_done_sem != NULL) {
        xSemaphoreGive(scan_done_sem);
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (scan_task_handle != NULL && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (scan_task_handle != NULL) {
        SOLAR_OS_LOGW(TAG, "sleep: forcing scan task stop");
        TaskHandle_t task = scan_task_handle;
        scan_task_handle = NULL;
        vTaskDelete(task);
    }

    pairing_cancel_requested = false;
    active_scan_results = NULL;
    active_scan_max_results = 0;
    active_scan_result_count = 0;
    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
    if (!connected) {
        set_status(BLE_KEYBOARD_IDLE, "sleep");
    }
}

static void gatt_lock(void)
{
    if (gatt_mutex != NULL) {
        xSemaphoreTake(gatt_mutex, portMAX_DELAY);
    }
}

static void gatt_unlock(void)
{
    if (gatt_mutex != NULL) {
        xSemaphoreGive(gatt_mutex);
    }
}

static void gatt_set_status_locked(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(gatt_state.status, sizeof(gatt_state.status), fmt, args);
    va_end(args);
}

static void reset_gatt_runtime_state(const char *status)
{
    gatt_lock();
    gatt_state.connected = false;
    gatt_state.registered = false;
    gatt_state.connecting = false;
    gatt_state.gattc_if = ESP_GATT_IF_NONE;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.mtu = 0;
    gatt_state.service_count = 0;
    gatt_state.op = BLE_GATT_OP_NONE;
    gatt_state.op_status = ESP_GATT_OK;
    gatt_state.op_value_len = 0;
    memset(gatt_state.bda, 0, sizeof(gatt_state.bda));
    gatt_set_status_locked("%s", status != NULL ? status : "idle");
    gatt_unlock();
}

static esp_err_t ensure_runtime_objects(void)
{
    if (scan_done_sem == NULL) {
        scan_done_sem = xSemaphoreCreateBinary();
    }
    if (close_done_sem == NULL) {
        close_done_sem = xSemaphoreCreateBinary();
    }
    if (status_mutex == NULL) {
        status_mutex = xSemaphoreCreateMutex();
    }
    if (gatt_mutex == NULL) {
        gatt_mutex = xSemaphoreCreateMutex();
    }
    if (gatt_op_sem == NULL) {
        gatt_op_sem = xSemaphoreCreateBinary();
    }
    if (char_queue == NULL) {
        char_queue = xQueueCreate(BLE_KEYBOARD_CHAR_QUEUE_LEN, sizeof(char));
    }

    if (status_mutex == NULL ||
        scan_done_sem == NULL ||
        close_done_sem == NULL ||
        gatt_mutex == NULL ||
        gatt_op_sem == NULL ||
        char_queue == NULL) {
        set_status(BLE_KEYBOARD_FAILED, "ble no memory");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void gatt_clear_services_locked(void)
{
    memset(gatt_state.services, 0, sizeof(gatt_state.services));
    gatt_state.service_count = 0;
}

static void gatt_uuid_to_string(const esp_bt_uuid_t *uuid, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    if (uuid == NULL) {
        strlcpy(buffer, "-", buffer_len);
        return;
    }

    switch (uuid->len) {
    case ESP_UUID_LEN_16:
        snprintf(buffer, buffer_len, "0x%04x", (unsigned)uuid->uuid.uuid16);
        break;
    case ESP_UUID_LEN_32:
        snprintf(buffer, buffer_len, "0x%08" PRIx32, uuid->uuid.uuid32);
        break;
    case ESP_UUID_LEN_128:
        snprintf(buffer,
                 buffer_len,
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 uuid->uuid.uuid128[15],
                 uuid->uuid.uuid128[14],
                 uuid->uuid.uuid128[13],
                 uuid->uuid.uuid128[12],
                 uuid->uuid.uuid128[11],
                 uuid->uuid.uuid128[10],
                 uuid->uuid.uuid128[9],
                 uuid->uuid.uuid128[8],
                 uuid->uuid.uuid128[7],
                 uuid->uuid.uuid128[6],
                 uuid->uuid.uuid128[5],
                 uuid->uuid.uuid128[4],
                 uuid->uuid.uuid128[3],
                 uuid->uuid.uuid128[2],
                 uuid->uuid.uuid128[1],
                 uuid->uuid.uuid128[0]);
        break;
    default:
        snprintf(buffer, buffer_len, "uuid-len-%u", (unsigned)uuid->len);
        break;
    }
}

static void gatt_drain_op_sem(void)
{
    if (gatt_op_sem == NULL) {
        return;
    }
    while (xSemaphoreTake(gatt_op_sem, 0) == pdTRUE) {
    }
}

static void gatt_complete_operation(ble_gatt_operation_t op, esp_gatt_status_t status)
{
    bool should_signal = false;

    gatt_lock();
    if (gatt_state.op == op || op == BLE_GATT_OP_NONE) {
        gatt_state.op_status = status;
        gatt_state.op = BLE_GATT_OP_NONE;
        should_signal = true;
    }
    gatt_unlock();

    if (should_signal && gatt_op_sem != NULL) {
        xSemaphoreGive(gatt_op_sem);
    }
}

static esp_err_t gatt_begin_operation(ble_gatt_operation_t op)
{
    gatt_lock();
    if (gatt_state.op != BLE_GATT_OP_NONE) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    gatt_state.op = op;
    gatt_state.op_status = ESP_GATT_OK;
    gatt_state.op_value_len = 0;
    gatt_unlock();
    gatt_drain_op_sem();
    return ESP_OK;
}

static esp_err_t gatt_wait_operation(ble_gatt_operation_t op, uint32_t timeout_ms)
{
    if (gatt_op_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(gatt_op_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        gatt_lock();
        if (gatt_state.op == op) {
            gatt_state.op = BLE_GATT_OP_NONE;
            gatt_set_status_locked("timeout");
        }
        gatt_unlock();
        return ESP_ERR_TIMEOUT;
    }

    esp_gatt_status_t status = ESP_GATT_OK;
    gatt_lock();
    status = gatt_state.op_status;
    gatt_unlock();
    return status == ESP_GATT_OK ? ESP_OK : ESP_FAIL;
}

static bool remembered_peer_valid_at(size_t index)
{
    return index < BLE_KEYBOARD_MAX_REMEMBERED &&
        remembered_peers[index].magic == BLE_KEYBOARD_PEER_MAGIC;
}

static size_t remembered_peer_count(void)
{
    size_t count = 0;

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_REMEMBERED; i++) {
        if (remembered_peer_valid_at(i)) {
            count++;
        }
    }

    return count;
}

static const ble_keyboard_peer_t *primary_remembered_peer(void)
{
    return remembered_peer_valid_at(0) ? &remembered_peers[0] : NULL;
}

static int remembered_peer_index_by_bda(const uint8_t *bda)
{
    if (bda == NULL) {
        return -1;
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_REMEMBERED; i++) {
        if (remembered_peer_valid_at(i) &&
            memcmp(bda, remembered_peers[i].bda, sizeof(remembered_peers[i].bda)) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int remembered_peer_index_by_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_REMEMBERED; i++) {
        if (remembered_peer_valid_at(i) &&
            remembered_peers[i].name[0] != '\0' &&
            strcmp(name, remembered_peers[i].name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static const ble_keyboard_peer_t *remembered_peer_for_bda(const uint8_t *bda)
{
    const int index = remembered_peer_index_by_bda(bda);
    return index >= 0 ? &remembered_peers[index] : NULL;
}

static bool bda_matches_remembered_peer(const uint8_t *bda)
{
    return remembered_peer_index_by_bda(bda) >= 0;
}

static bool name_matches_remembered_peer(const char *name)
{
    return remembered_peer_index_by_name(name) >= 0;
}

static esp_err_t load_keyboard_layout(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t value = 0;
    ret = nvs_get_u16(nvs, BLE_KEYBOARD_NVS_LAYOUT_KEY, &value);
    nvs_close(nvs);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }
    if (value >= sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0])) {
        return ESP_ERR_INVALID_ARG;
    }

    keyboard_layout = (solar_os_ble_keyboard_layout_t)value;
    return ESP_OK;
}

static esp_err_t save_keyboard_layout(solar_os_ble_keyboard_layout_t layout)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u16(nvs, BLE_KEYBOARD_NVS_LAYOUT_KEY, (uint16_t)layout);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static bool repeat_config_valid(uint16_t rate_cps, uint16_t delay_ms)
{
    if (rate_cps > SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MAX) {
        return false;
    }
    if (rate_cps != 0 && rate_cps < SOLAR_OS_BLE_KEYBOARD_REPEAT_RATE_MIN) {
        return false;
    }

    return delay_ms >= SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MIN_MS &&
        delay_ms <= SOLAR_OS_BLE_KEYBOARD_REPEAT_DELAY_MAX_MS;
}

static esp_err_t load_keyboard_repeat(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t rate = repeat_rate_cps;
    uint16_t delay = repeat_delay_ms;
    ret = nvs_get_u16(nvs, BLE_KEYBOARD_NVS_REPEAT_RATE_KEY, &rate);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        esp_err_t delay_ret = nvs_get_u16(nvs, BLE_KEYBOARD_NVS_REPEAT_DELAY_KEY, &delay);
        if (delay_ret == ESP_ERR_NVS_NOT_FOUND) {
            delay_ret = ESP_OK;
        }
        ret = delay_ret;
    }
    nvs_close(nvs);

    if (ret != ESP_OK) {
        return ret;
    }
    if (!repeat_config_valid(rate, delay)) {
        return ESP_ERR_INVALID_ARG;
    }

    repeat_rate_cps = rate;
    repeat_delay_ms = delay;
    return ESP_OK;
}

static esp_err_t save_keyboard_repeat(uint16_t rate_cps, uint16_t delay_ms)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u16(nvs, BLE_KEYBOARD_NVS_REPEAT_RATE_KEY, rate_cps);
    if (ret == ESP_OK) {
        ret = nvs_set_u16(nvs, BLE_KEYBOARD_NVS_REPEAT_DELAY_KEY, delay_ms);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t save_remembered_peers_to_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "open BLE keyboard NVS failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(nvs,
                       BLE_KEYBOARD_NVS_PEERS_KEY,
                       remembered_peers,
                       sizeof(remembered_peers));
    if (ret == ESP_OK) {
        ret = nvs_erase_key(nvs, BLE_KEYBOARD_NVS_LEGACY_PEER_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "save BLE keyboard peers failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static void log_remembered_peers(void)
{
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_REMEMBERED; i++) {
        if (!remembered_peer_valid_at(i)) {
            continue;
        }

        SOLAR_OS_LOGI(TAG,
                 "remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
                 ESP_BD_ADDR_HEX(remembered_peers[i].bda),
                 addr_type_name((esp_ble_addr_type_t)remembered_peers[i].addr_type),
                 remembered_peers[i].name[0] ? remembered_peers[i].name : "(unnamed)");
    }
}

static esp_err_t load_remembered_peers(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    memset(remembered_peers, 0, sizeof(remembered_peers));
    ble_keyboard_peer_t loaded_peers[BLE_KEYBOARD_NVS_MIGRATE_MAX_REMEMBERED] = {0};
    size_t len = sizeof(loaded_peers);
    ret = nvs_get_blob(nvs, BLE_KEYBOARD_NVS_PEERS_KEY, loaded_peers, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ble_keyboard_peer_t legacy_peer = {0};
        len = sizeof(legacy_peer);
        ret = nvs_get_blob(nvs, BLE_KEYBOARD_NVS_LEGACY_PEER_KEY, &legacy_peer, &len);
        if (ret == ESP_OK && len == sizeof(legacy_peer) &&
            legacy_peer.magic == BLE_KEYBOARD_PEER_MAGIC) {
            remembered_peers[0] = legacy_peer;
            nvs_close(nvs);
            SOLAR_OS_LOGI(TAG, "migrating legacy BLE keyboard peer");
            (void)save_remembered_peers_to_nvs();
            log_remembered_peers();
            return ESP_OK;
        }
    }
    nvs_close(nvs);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
        return ret;
    }
    if (len < sizeof(ble_keyboard_peer_t) ||
        (len % sizeof(ble_keyboard_peer_t)) != 0 ||
        len > sizeof(loaded_peers)) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t loaded_count = len / sizeof(ble_keyboard_peer_t);
    for (size_t i = 0; i < loaded_count; i++) {
        if (loaded_peers[i].magic == BLE_KEYBOARD_PEER_MAGIC) {
            remembered_peers[0] = loaded_peers[i];
            break;
        }
    }
    log_remembered_peers();
    if (len != sizeof(remembered_peers)) {
        SOLAR_OS_LOGI(TAG, "migrating BLE keyboard peers to single remembered keyboard");
        (void)save_remembered_peers_to_nvs();
    }
    return ESP_OK;
}

static esp_err_t save_remembered_peer(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name)
{
    if (bda == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ble_keyboard_peer_t peer = {
        .magic = BLE_KEYBOARD_PEER_MAGIC,
        .addr_type = (uint8_t)addr_type,
    };
    memcpy(peer.bda, bda, sizeof(peer.bda));
    strlcpy(peer.name, name != NULL && name[0] ? name : "keyboard", sizeof(peer.name));

    memset(remembered_peers, 0, sizeof(remembered_peers));
    remembered_peers[0] = peer;

    const esp_err_t ret = save_remembered_peers_to_nvs();
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG,
                 "remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
                 ESP_BD_ADDR_HEX(peer.bda),
                 addr_type_name((esp_ble_addr_type_t)peer.addr_type),
                 peer.name);
    }

    return ret;
}

static esp_err_t clear_remembered_peers(void)
{
    memset(remembered_peers, 0, sizeof(remembered_peers));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(nvs, BLE_KEYBOARD_NVS_PEERS_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_erase_key(nvs, BLE_KEYBOARD_NVS_LEGACY_PEER_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static bool hidh_link_ready(esp_hidh_dev_t *dev, esp_gap_conn_params_t *params)
{
    if (dev == NULL || !esp_hidh_dev_exists(dev)) {
        return false;
    }

    const uint8_t *bda = esp_hidh_dev_bda_get(dev);
    if (bda == NULL) {
        return false;
    }

    esp_gap_conn_params_t local_params = {0};
    esp_gap_conn_params_t *out_params = params != NULL ? params : &local_params;
    const esp_err_t ret = esp_ble_get_current_conn_params((uint8_t *)bda, out_params);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "HID keyboard link has no BLE conn params: %s",
                      esp_err_to_name(ret));
        return false;
    }

    if (out_params->interval == 0 || out_params->timeout == 0) {
        SOLAR_OS_LOGW(TAG,
                      "HID keyboard link has invalid BLE conn params interval=%u timeout=%u",
                      (unsigned)out_params->interval,
                      (unsigned)out_params->timeout);
        return false;
    }

    return true;
}

static bool drop_existing_hidh_device(const uint8_t *bda, const char *reason, uint32_t timeout_ms)
{
    if (bda == NULL) {
        return true;
    }

    esp_bd_addr_t lookup_bda;
    memcpy(lookup_bda, bda, sizeof(lookup_bda));
    esp_hidh_dev_t *dev = esp_hidh_dev_get_by_bda(lookup_bda);
    if (dev == NULL) {
        return true;
    }

    SOLAR_OS_LOGW(TAG,
                  "%s: dropping HIDH keyboard " ESP_BD_ADDR_STR
                  " connected=%u status=0x%x conn_id=%d reports=%u",
                  reason != NULL ? reason : "ble",
                  ESP_BD_ADDR_HEX(bda),
                  dev->connected ? 1U : 0U,
                  (unsigned)dev->status,
                  dev->ble.conn_id,
                  (unsigned)dev->reports_len);

    if (connected_dev == dev) {
        connected = false;
        connected_dev = NULL;
    }
    if (pending_dev == dev) {
        pending_dev = NULL;
        pending_open_started_tick = 0;
    }

    if (dev->close == NULL || dev->ble.conn_id < 0) {
        (void)esp_hidh_dev_free_inner(dev);
        return true;
    }

    while (close_done_sem != NULL && xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
    }

    const esp_err_t close_ret = esp_hidh_dev_close(dev);
    if (close_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "%s: stale HIDH close failed: %s",
                      reason != NULL ? reason : "ble",
                      esp_err_to_name(close_ret));
        return false;
    }

    if (timeout_ms > 0 &&
        close_done_sem != NULL &&
        xSemaphoreTake(close_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        SOLAR_OS_LOGW(TAG,
                      "%s: stale HIDH close timeout",
                      reason != NULL ? reason : "ble");
        return false;
    }

    return true;
}

static esp_err_t open_keyboard(const uint8_t *bda,
                               esp_ble_addr_type_t addr_type,
                               const char *name,
                               const char *status_action)
{
    memcpy(pending_bda, bda, sizeof(pending_bda));
    pending_addr_type = addr_type;
    strlcpy(pending_name, name != NULL && name[0] ? name : "keyboard", sizeof(pending_name));
    set_status(BLE_KEYBOARD_CONNECTING, "%s %s", status_action, pending_name);

    if (!drop_existing_hidh_device(bda, status_action, BLE_KEYBOARD_STALE_CLOSE_TIMEOUT_MS)) {
        set_status(BLE_KEYBOARD_FAILED, "hid busy");
        return ESP_ERR_INVALID_STATE;
    }

    pending_open_started_tick = xTaskGetTickCount();
    esp_hidh_dev_t *opened_dev = esp_hidh_dev_open(pending_bda, ESP_HID_TRANSPORT_BLE, pending_addr_type);
    if (opened_dev == NULL) {
        pending_open_started_tick = 0;
        SOLAR_OS_LOGE(TAG, "esp_hidh_dev_open failed");
        set_status(BLE_KEYBOARD_FAILED, "connect failed");
        return ESP_FAIL;
    }
    if (connected && connected_dev == opened_dev) {
        pending_dev = NULL;
        pending_open_started_tick = 0;
    } else {
        pending_dev = opened_dev;
    }

    return ESP_OK;
}

static bool close_pending_open_attempt(const char *reason, uint32_t timeout_ms)
{
    if (pending_dev == NULL) {
        return true;
    }
    if (connected && pending_dev == connected_dev) {
        pending_dev = NULL;
        pending_open_started_tick = 0;
        return true;
    }

    while (close_done_sem != NULL && xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
    }

    SOLAR_OS_LOGI(TAG, "%s: closing pending keyboard open", reason != NULL ? reason : "ble");
    esp_hidh_dev_t *dev = pending_dev;
    pending_dev = NULL;
    pending_open_started_tick = 0;
    esp_err_t err = ESP_FAIL;
    if (esp_hidh_dev_exists(dev)) {
        err = esp_hidh_dev_close(dev);
    }
    if (err != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                      "%s: pending keyboard close failed: %s",
                      reason != NULL ? reason : "ble",
                      esp_err_to_name(err));
        if (!connected) {
            clear_runtime_connection_state(reason);
        }
        return false;
    }

    if (timeout_ms > 0 &&
        close_done_sem != NULL &&
        xSemaphoreTake(close_done_sem, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        SOLAR_OS_LOGW(TAG, "%s: pending keyboard close timeout", reason != NULL ? reason : "ble");
        if (!connected) {
            clear_runtime_connection_state(reason);
        }
        return false;
    }

    if (!connected && state == BLE_KEYBOARD_CONNECTING) {
        set_status(BLE_KEYBOARD_IDLE, "%s", reason != NULL ? reason : "idle");
    }
    return true;
}

void solar_os_ble_keyboard_get_status(char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    if (status_mutex != NULL) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }

    strlcpy(buffer, status_text, buffer_len);

    if (status_mutex != NULL) {
        xSemaphoreGive(status_mutex);
    }
}

bool solar_os_ble_keyboard_is_connected(void)
{
    return connected;
}

bool solar_os_ble_keyboard_is_scanning(void)
{
    bool scanning;

    if (status_mutex != NULL) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }

    scanning = state == BLE_KEYBOARD_SCANNING || state == BLE_KEYBOARD_PAIRING_PENDING;

    if (status_mutex != NULL) {
        xSemaphoreGive(status_mutex);
    }

    return scanning;
}

bool solar_os_ble_keyboard_is_pairing(void)
{
    bool pairing;

    if (status_mutex != NULL) {
        xSemaphoreTake(status_mutex, portMAX_DELAY);
    }

    pairing = pairing_retry_pending ||
        state == BLE_KEYBOARD_PAIRING_PENDING ||
        (state == BLE_KEYBOARD_SCANNING && active_scan_mode == BLE_KEYBOARD_SCAN_PAIRING);

    if (status_mutex != NULL) {
        xSemaphoreGive(status_mutex);
    }

    return pairing;
}

size_t solar_os_ble_keyboard_remembered_count(void)
{
    return remembered_peer_count();
}

size_t solar_os_ble_keyboard_read_chars(char *buffer, size_t buffer_len)
{
    size_t count = 0;

    if (buffer == NULL || buffer_len == 0 || char_queue == NULL) {
        return 0;
    }

    repeat_queue_if_due();

    while (count < buffer_len && xQueueReceive(char_queue, &buffer[count], 0) == pdTRUE) {
        count++;
    }

    return count;
}

void solar_os_ble_keyboard_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms)
{
    portENTER_CRITICAL(&repeat_lock);
    if (rate_cps != NULL) {
        *rate_cps = repeat_rate_cps;
    }
    if (delay_ms != NULL) {
        *delay_ms = repeat_delay_ms;
    }
    portEXIT_CRITICAL(&repeat_lock);
}

esp_err_t solar_os_ble_keyboard_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{
    if (delay_ms == 0) {
        solar_os_ble_keyboard_get_repeat(NULL, &delay_ms);
    }
    if (!repeat_config_valid(rate_cps, delay_ms)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&repeat_lock);
    repeat_rate_cps = rate_cps;
    repeat_delay_ms = delay_ms;
    if (repeat_rate_cps == 0) {
        repeat_state.active = false;
    }
    portEXIT_CRITICAL(&repeat_lock);

    return save_keyboard_repeat(rate_cps, delay_ms);
}

solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void)
{
    return keyboard_layout;
}

esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    if (keyboard_layout == layout) {
        return ESP_OK;
    }

    keyboard_layout = layout;
    return save_keyboard_layout(layout);
}

const char *solar_os_ble_keyboard_layout_name(solar_os_ble_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0])) {
        return "unknown";
    }

    return keyboard_layout_names[layout];
}

bool solar_os_ble_keyboard_parse_layout(const char *name, solar_os_ble_keyboard_layout_t *layout)
{
    if (name == NULL || layout == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0]); i++) {
        if (strcmp(name, keyboard_layout_names[i]) == 0) {
            *layout = (solar_os_ble_keyboard_layout_t)i;
            return true;
        }
    }

    return false;
}

const char *solar_os_ble_keyboard_addr_type_name(uint8_t addr_type)
{
    return addr_type_name((esp_ble_addr_type_t)addr_type);
}

bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type)
{
    if (name == NULL || addr_type == NULL) {
        return false;
    }

    const esp_ble_addr_type_t types[] = {
        BLE_ADDR_TYPE_PUBLIC,
        BLE_ADDR_TYPE_RANDOM,
        BLE_ADDR_TYPE_RPA_PUBLIC,
        BLE_ADDR_TYPE_RPA_RANDOM,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        const char *type_name = addr_type_name(types[i]);
        if (strcmp(name, type_name) == 0) {
            *addr_type = (uint8_t)types[i];
            return true;
        }
    }
    return false;
}

static const char *addr_type_name(esp_ble_addr_type_t addr_type)
{
    switch (addr_type) {
    case BLE_ADDR_TYPE_PUBLIC:
        return "public";
    case BLE_ADDR_TYPE_RANDOM:
        return "random";
    case BLE_ADDR_TYPE_RPA_PUBLIC:
        return "rpa_public";
    case BLE_ADDR_TYPE_RPA_RANDOM:
        return "rpa_random";
    default:
        return "unknown";
    }
}

static bool contains_ci(const char *haystack, const char *needle)
{
    const size_t needle_len = strlen(needle);

    if (needle_len == 0) {
        return true;
    }

    for (const char *h = haystack; *h != '\0'; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] != '\0' &&
               (char)tolower((unsigned char)h[i]) == needle[i]) {
            i++;
        }

        if (i == needle_len) {
            return true;
        }
    }

    return false;
}

static bool adv_has_uuid16(uint8_t *adv_data, uint16_t adv_len, esp_ble_adv_data_type type, uint16_t uuid)
{
    uint8_t uuid_len = 0;
    uint8_t *uuid_data = esp_ble_resolve_adv_data_by_type(adv_data, adv_len, type, &uuid_len);
    if (uuid_data == NULL || uuid_len < 2) {
        return false;
    }

    for (uint8_t i = 0; i + 1 < uuid_len; i += 2) {
        const uint16_t found = (uint16_t)uuid_data[i] | ((uint16_t)uuid_data[i + 1] << 8);
        if (found == uuid) {
            return true;
        }
    }

    return false;
}

static uint16_t adv_appearance(uint8_t *adv_data, uint16_t adv_len)
{
    uint8_t appearance_len = 0;
    uint8_t *appearance = esp_ble_resolve_adv_data_by_type(
        adv_data, adv_len, ESP_BLE_AD_TYPE_APPEARANCE, &appearance_len);

    if (appearance == NULL || appearance_len < 2) {
        return 0;
    }

    return (uint16_t)appearance[0] | ((uint16_t)appearance[1] << 8);
}

static void adv_name(uint8_t *adv_data, uint16_t adv_len, char *name, size_t name_len)
{
    uint8_t raw_len = 0;
    uint8_t *raw_name = esp_ble_resolve_adv_data_by_type(
        adv_data, adv_len, ESP_BLE_AD_TYPE_NAME_CMPL, &raw_len);

    if (raw_name == NULL) {
        raw_name = esp_ble_resolve_adv_data_by_type(
            adv_data, adv_len, ESP_BLE_AD_TYPE_NAME_SHORT, &raw_len);
    }

    if (raw_name == NULL || raw_len == 0 || name_len == 0) {
        if (name_len > 0) {
            name[0] = '\0';
        }
        return;
    }

    const size_t copy_len = raw_len < (name_len - 1) ? raw_len : (name_len - 1);
    memcpy(name, raw_name, copy_len);
    name[copy_len] = '\0';
}

static bool is_keyboard_like(uint16_t appearance, const char *name)
{
    if (appearance == ESP_HID_APPEARANCE_KEYBOARD) {
        return true;
    }

    return contains_ci(name, "keyboard") ||
           contains_ci(name, "kbd") ||
           contains_ci(name, "keychron");
}

static solar_os_ble_keyboard_scan_result_t *scan_result_slot(const uint8_t *bda)
{
    if (active_scan_results == NULL || active_scan_max_results == 0 || bda == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < active_scan_result_count; i++) {
        if (memcmp(active_scan_results[i].bda, bda, sizeof(active_scan_results[i].bda)) == 0) {
            return &active_scan_results[i];
        }
    }

    if (active_scan_result_count >= active_scan_max_results) {
        return NULL;
    }

    return &active_scan_results[active_scan_result_count++];
}

static void collect_scan_result(const uint8_t *bda,
                                esp_ble_addr_type_t addr_type,
                                int8_t rssi,
                                uint16_t appearance,
                                bool hid_service,
                                bool keyboard_like,
                                const char *name)
{
    solar_os_ble_keyboard_scan_result_t *slot = scan_result_slot(bda);
    if (slot == NULL) {
        return;
    }

    memcpy(slot->bda, bda, sizeof(slot->bda));
    slot->addr_type = (uint8_t)addr_type;
    slot->rssi = rssi;
    slot->appearance = appearance;
    slot->hid_service = slot->hid_service || hid_service;
    slot->keyboard_like = slot->keyboard_like || keyboard_like;
    slot->remembered = slot->remembered ||
        bda_matches_remembered_peer(bda) ||
        name_matches_remembered_peer(name);
    if (name != NULL && name[0] != '\0') {
        strlcpy(slot->name, name, sizeof(slot->name));
    }
}

static void collect_connected_scan_result(void)
{
    if (!connected || connected_dev == NULL) {
        return;
    }

    const uint8_t *bda = esp_hidh_dev_bda_get(connected_dev);
    if (bda == NULL) {
        return;
    }

    solar_os_ble_keyboard_scan_result_t *slot = scan_result_slot(bda);
    if (slot == NULL) {
        return;
    }

    const char *name = esp_hidh_dev_name_get(connected_dev);
    if (name == NULL || name[0] == '\0') {
        name = connected_name;
    }

    memcpy(slot->bda, bda, sizeof(slot->bda));
    const ble_keyboard_peer_t *peer = remembered_peer_for_bda(bda);
    slot->addr_type = peer != NULL ? peer->addr_type : (uint8_t)pending_addr_type;
    slot->rssi = 0;
    slot->appearance = ESP_HID_APPEARANCE_KEYBOARD;
    slot->hid_service = true;
    slot->keyboard_like = true;
    slot->remembered = true;
    slot->connected = true;
    strlcpy(slot->name, name != NULL && name[0] != '\0' ? name : "keyboard", sizeof(slot->name));
}

static void consider_candidate(const esp_ble_gap_cb_param_t *param)
{
    char name[BLE_KEYBOARD_NAME_MAX];
    const uint16_t adv_len = param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
    uint8_t *adv_data = (uint8_t *)param->scan_rst.ble_adv;
    const uint16_t appearance = adv_appearance(adv_data, adv_len);
    const bool has_hid_service =
        adv_has_uuid16(adv_data, adv_len, ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_GATT_UUID_HID_SVC) ||
        adv_has_uuid16(adv_data, adv_len, ESP_BLE_AD_TYPE_16SRV_PART, ESP_GATT_UUID_HID_SVC);

    adv_name(adv_data, adv_len, name, sizeof(name));

    const bool keyboard_like = is_keyboard_like(appearance, name);
    if (active_scan_mode == BLE_KEYBOARD_SCAN_DISCOVERY) {
        collect_scan_result(param->scan_rst.bda,
                            param->scan_rst.ble_addr_type,
                            param->scan_rst.rssi,
                            appearance,
                            has_hid_service,
                            keyboard_like,
                            name);
    }

    if (!has_hid_service && !keyboard_like) {
        return;
    }

    if (candidate.valid) {
        if (candidate.keyboard_like && !keyboard_like) {
            return;
        }
        if (candidate.keyboard_like == keyboard_like &&
            param->scan_rst.rssi <= candidate.rssi) {
            return;
        }
    }

    candidate.valid = true;
    candidate.keyboard_like = keyboard_like;
    memcpy(candidate.bda, param->scan_rst.bda, sizeof(candidate.bda));
    candidate.addr_type = param->scan_rst.ble_addr_type;
    candidate.rssi = param->scan_rst.rssi;
    candidate.appearance = appearance;
    strlcpy(candidate.name, name, sizeof(candidate.name));

    SOLAR_OS_LOGI(TAG,
             "candidate " ESP_BD_ADDR_STR " rssi=%d appearance=0x%04x addr_type=%s name=%s%s",
             ESP_BD_ADDR_HEX(candidate.bda),
             candidate.rssi,
             candidate.appearance,
             addr_type_name(candidate.addr_type),
             candidate.name[0] ? candidate.name : "(none)",
             candidate.keyboard_like ? " keyboard-like" : "");
}

static const char *key_type_name(esp_ble_key_type_t key_type)
{
    switch (key_type) {
    case ESP_LE_KEY_NONE:
        return "none";
    case ESP_LE_KEY_PENC:
        return "penc";
    case ESP_LE_KEY_PID:
        return "pid";
    case ESP_LE_KEY_PCSRK:
        return "pcsrk";
    case ESP_LE_KEY_PLK:
        return "plk";
    case ESP_LE_KEY_LLK:
        return "llk";
    case ESP_LE_KEY_LENC:
        return "lenc";
    case ESP_LE_KEY_LID:
        return "lid";
    case ESP_LE_KEY_LCSRK:
        return "lcsrk";
    default:
        return "unknown";
    }
}

static bool key_in_report(uint8_t key, const uint8_t *keys)
{
    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        if (keys[i] == key) {
            return true;
        }
    }

    return false;
}

static void queue_char(char ch)
{
    if (char_queue == NULL) {
        return;
    }

    if (xQueueSend(char_queue, &ch, 0) != pdTRUE) {
        SOLAR_OS_LOGW(TAG, "keyboard char queue full");
    }
}

static uint32_t keyboard_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static uint32_t repeat_interval_ms(uint16_t rate_cps)
{
    if (rate_cps == 0) {
        return 0;
    }

    uint32_t interval = (1000U + rate_cps - 1U) / rate_cps;
    return interval > 0 ? interval : 1U;
}

static bool repeat_time_reached(uint32_t now_ms, uint32_t target_ms)
{
    return (int32_t)(now_ms - target_ms) >= 0;
}

static void repeat_clear(void)
{
    portENTER_CRITICAL(&repeat_lock);
    repeat_state.active = false;
    portEXIT_CRITICAL(&repeat_lock);
}

static bool repeatable_char(char ch)
{
    return ch != '\0' && (uint8_t)ch != SOLAR_OS_KEY_APP_EXIT;
}

static void repeat_start(uint8_t keycode, const char *sequence, size_t sequence_len)
{
    if (sequence == NULL || sequence_len == 0 ||
        sequence_len > BLE_KEYBOARD_REPEAT_SEQUENCE_MAX) {
        repeat_clear();
        return;
    }

    const uint32_t now_ms = keyboard_now_ms();

    portENTER_CRITICAL(&repeat_lock);
    if (repeat_rate_cps == 0) {
        repeat_state.active = false;
        portEXIT_CRITICAL(&repeat_lock);
        return;
    }

    repeat_state.active = true;
    repeat_state.keycode = keycode;
    repeat_state.sequence_len = (uint8_t)sequence_len;
    memcpy(repeat_state.sequence, sequence, sequence_len);
    repeat_state.next_ms = now_ms + repeat_delay_ms;
    portEXIT_CRITICAL(&repeat_lock);
}

static void repeat_stop_if_released(const uint8_t *keys)
{
    portENTER_CRITICAL(&repeat_lock);
    if (repeat_state.active && !key_in_report(repeat_state.keycode, keys)) {
        repeat_state.active = false;
    }
    portEXIT_CRITICAL(&repeat_lock);
}

static void repeat_queue_if_due(void)
{
    char sequence[BLE_KEYBOARD_REPEAT_SEQUENCE_MAX];
    uint8_t sequence_len = 0;
    bool due = false;
    const uint32_t now_ms = keyboard_now_ms();

    portENTER_CRITICAL(&repeat_lock);
    if (repeat_state.active && repeat_rate_cps != 0 &&
        repeat_time_reached(now_ms, repeat_state.next_ms)) {
        sequence_len = repeat_state.sequence_len;
        memcpy(sequence, repeat_state.sequence, sequence_len);
        repeat_state.next_ms = now_ms + repeat_interval_ms(repeat_rate_cps);
        due = true;
    }
    portEXIT_CRITICAL(&repeat_lock);

    if (!due) {
        return;
    }

    for (uint8_t i = 0; i < sequence_len; i++) {
        queue_char(sequence[i]);
    }
}

static char shifted_digit(uint8_t keycode)
{
    static const char shifted[] = {
        [0x1e - 0x1e] = '!',
        [0x1f - 0x1e] = '@',
        [0x20 - 0x1e] = '#',
        [0x21 - 0x1e] = '$',
        [0x22 - 0x1e] = '%',
        [0x23 - 0x1e] = '^',
        [0x24 - 0x1e] = '&',
        [0x25 - 0x1e] = '*',
        [0x26 - 0x1e] = '(',
        [0x27 - 0x1e] = ')',
    };

    return shifted[keycode - 0x1e];
}

static char unshifted_digit(uint8_t keycode)
{
    return keycode == 0x27 ? '0' : (char)('1' + (keycode - 0x1e));
}

static char hid_keycode_to_char_us(uint8_t keycode, bool shift)
{
    if (keycode >= 0x04 && keycode <= 0x1d) {
        const bool upper = shift ^ caps_lock;
        return (char)((upper ? 'A' : 'a') + (keycode - 0x04));
    }

    if (keycode >= 0x1e && keycode <= 0x27) {
        return shift ? shifted_digit(keycode) : unshifted_digit(keycode);
    }

    switch (keycode) {
    case 0x29:
        return SOLAR_OS_KEY_ESCAPE;
    case 0x28:
        return '\n';
    case 0x2a:
        return '\b';
    case 0x2b:
        return '\t';
    case 0x2c:
        return ' ';
    case 0x2d:
        return shift ? '_' : '-';
    case 0x2e:
        return shift ? '+' : '=';
    case 0x2f:
        return shift ? '{' : '[';
    case 0x30:
        return shift ? '}' : ']';
    case 0x31:
        return shift ? '|' : '\\';
    case 0x32:
        return shift ? '~' : '#';
    case 0x33:
        return shift ? ':' : ';';
    case 0x34:
        return shift ? '"' : '\'';
    case 0x35:
        return shift ? '~' : '`';
    case 0x36:
        return shift ? '<' : ',';
    case 0x37:
        return shift ? '>' : '.';
    case 0x38:
        return shift ? '?' : '/';
    case 0x4b:
        return SOLAR_OS_KEY_PAGE_UP;
    case 0x4e:
        return SOLAR_OS_KEY_PAGE_DOWN;
    case 0x4f:
        return SOLAR_OS_KEY_RIGHT;
    case 0x50:
        return SOLAR_OS_KEY_LEFT;
    case 0x51:
        return SOLAR_OS_KEY_DOWN;
    case 0x52:
        return SOLAR_OS_KEY_UP;
    default:
        return '\0';
    }
}

static char hid_keycode_to_char_de(uint8_t keycode, uint8_t modifiers)
{
    const bool shift = (modifiers & HID_MOD_SHIFT) != 0;
    const bool altgr = (modifiers & 0x40) != 0;

    if (altgr) {
        switch (keycode) {
        case 0x14:
            return '@';
        case 0x24:
            return '{';
        case 0x25:
            return '[';
        case 0x26:
            return ']';
        case 0x27:
            return '}';
        case 0x2d:
            return '\\';
        case 0x30:
            return '~';
        case 0x64:
            return '|';
        default:
            return '\0';
        }
    }

    if (keycode >= 0x04 && keycode <= 0x1d) {
        const bool upper = shift ^ caps_lock;
        char base = (char)('a' + (keycode - 0x04));
        if (base == 'y') {
            base = 'z';
        } else if (base == 'z') {
            base = 'y';
        }
        return upper ? (char)toupper((unsigned char)base) : base;
    }

    if (keycode >= 0x1e && keycode <= 0x27) {
        static const char shifted[] = {
            [0x1e - 0x1e] = '!',
            [0x1f - 0x1e] = '"',
            [0x20 - 0x1e] = '#',
            [0x21 - 0x1e] = '$',
            [0x22 - 0x1e] = '%',
            [0x23 - 0x1e] = '&',
            [0x24 - 0x1e] = '/',
            [0x25 - 0x1e] = '(',
            [0x26 - 0x1e] = ')',
            [0x27 - 0x1e] = '=',
        };
        return shift ? shifted[keycode - 0x1e] : unshifted_digit(keycode);
    }

    switch (keycode) {
    case 0x29:
        return SOLAR_OS_KEY_ESCAPE;
    case 0x28:
        return '\n';
    case 0x2a:
        return '\b';
    case 0x2b:
        return '\t';
    case 0x2c:
        return ' ';
    case 0x2d:
        return shift ? '?' : LATIN1_SHARP_S;
    case 0x2e:
        return shift ? '`' : '\0';
    case 0x2f:
        return shift ? LATIN1_U_UMLAUT_UPPER : LATIN1_U_UMLAUT_LOWER;
    case 0x30:
        return shift ? '*' : '+';
    case 0x31:
        return shift ? '\'' : '#';
    case 0x32:
        return shift ? '\'' : '#';
    case 0x33:
        return shift ? LATIN1_O_UMLAUT_UPPER : LATIN1_O_UMLAUT_LOWER;
    case 0x34:
        return shift ? LATIN1_A_UMLAUT_UPPER : LATIN1_A_UMLAUT_LOWER;
    case 0x35:
        return shift ? '\0' : '^';
    case 0x36:
        return shift ? ';' : ',';
    case 0x37:
        return shift ? ':' : '.';
    case 0x38:
        return shift ? '_' : '-';
    case 0x4b:
        return SOLAR_OS_KEY_PAGE_UP;
    case 0x4e:
        return SOLAR_OS_KEY_PAGE_DOWN;
    case 0x4f:
        return SOLAR_OS_KEY_RIGHT;
    case 0x50:
        return SOLAR_OS_KEY_LEFT;
    case 0x51:
        return SOLAR_OS_KEY_DOWN;
    case 0x52:
        return SOLAR_OS_KEY_UP;
    case 0x64:
        return shift ? '>' : '<';
    default:
        return '\0';
    }
}

static char hid_keycode_to_function_key(uint8_t keycode)
{
    switch (keycode) {
    case 0x3a:
        return SOLAR_OS_KEY_F1;
    case 0x3b:
        return SOLAR_OS_KEY_F2;
    case 0x3c:
        return SOLAR_OS_KEY_F3;
    case 0x3d:
        return SOLAR_OS_KEY_F4;
    case 0x3e:
        return SOLAR_OS_KEY_F5;
    case 0x3f:
        return SOLAR_OS_KEY_F6;
    case 0x40:
        return SOLAR_OS_KEY_F7;
    case 0x41:
        return SOLAR_OS_KEY_F8;
    case 0x42:
        return SOLAR_OS_KEY_F9;
    case 0x43:
        return SOLAR_OS_KEY_F10;
    case 0x44:
        return SOLAR_OS_KEY_F11;
    case 0x45:
        return SOLAR_OS_KEY_F12;
    default:
        return '\0';
    }
}

static char hid_keycode_to_nav_key(uint8_t keycode, uint8_t modifiers)
{
    const bool ctrl = (modifiers & HID_MOD_CTRL) != 0;
    const bool shift = (modifiers & HID_MOD_SHIFT) != 0;

    switch (keycode) {
    case 0x4a:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_HOME;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_HOME;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_HOME : SOLAR_OS_KEY_HOME;
    case 0x4b:
        return shift ? SOLAR_OS_KEY_SHIFT_PAGE_UP : SOLAR_OS_KEY_PAGE_UP;
    case 0x4c:
        return SOLAR_OS_KEY_DELETE;
    case 0x4d:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_END;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_END;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_END : SOLAR_OS_KEY_END;
    case 0x4e:
        return shift ? SOLAR_OS_KEY_SHIFT_PAGE_DOWN : SOLAR_OS_KEY_PAGE_DOWN;
    case 0x4f:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_RIGHT;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_RIGHT;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_RIGHT : SOLAR_OS_KEY_RIGHT;
    case 0x50:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_LEFT;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_LEFT;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_LEFT : SOLAR_OS_KEY_LEFT;
    case 0x51:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_DOWN;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_DOWN;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_DOWN : SOLAR_OS_KEY_DOWN;
    case 0x52:
        if (ctrl && shift) {
            return SOLAR_OS_KEY_CTRL_SHIFT_UP;
        }
        if (ctrl) {
            return SOLAR_OS_KEY_CTRL_UP;
        }
        return shift ? SOLAR_OS_KEY_SHIFT_UP : SOLAR_OS_KEY_UP;
    default:
        return '\0';
    }
}

static char hid_keycode_to_control_char(uint8_t keycode)
{
    if (keycode >= 0x04 && keycode <= 0x1d) {
        char base = (char)('a' + (keycode - 0x04));
        if (keyboard_layout == SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE) {
            if (base == 'y') {
                base = 'z';
            } else if (base == 'z') {
                base = 'y';
            }
        }
        return (char)(base - 'a' + 1);
    }

    switch (keycode) {
    case 0x23:
        return 0x1e;
    case 0x2d:
        return 0x1f;
    case 0x30:
        return 0x1d;
    case 0x31:
        return 0x1c;
    default:
        return '\0';
    }
}

static char hid_keycode_to_system_key(uint8_t keycode, uint8_t modifiers)
{
    const bool ctrl = (modifiers & HID_MOD_CTRL) != 0;
    const bool alt = (modifiers & HID_MOD_ALT) != 0;

    if (ctrl && alt && keycode == 0x4c) {
        SOLAR_OS_LOGI(TAG, "mapped CTRL+ALT+DEL to app-exit key");
        return (char)SOLAR_OS_KEY_APP_EXIT;
    }
    if (ctrl && !alt) {
        if (keycode == 0x2e ||
            (keyboard_layout == SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE && keycode == 0x30)) {
            return (char)SOLAR_OS_KEY_CTRL_PLUS;
        }
        if (keyboard_layout == SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE && keycode == 0x38) {
            return 0x1f;
        }
    }

    return '\0';
}

static char hid_keycode_to_char(uint8_t keycode, uint8_t modifiers)
{
    const bool shift = (modifiers & HID_MOD_SHIFT) != 0;
    const char system_key = hid_keycode_to_system_key(keycode, modifiers);
    const char function_key = hid_keycode_to_function_key(keycode);
    const char nav_key = hid_keycode_to_nav_key(keycode, modifiers);

    if (system_key != '\0') {
        return system_key;
    }
    if (function_key != '\0') {
        return function_key;
    }
    if (nav_key != '\0') {
        return nav_key;
    }
    if ((modifiers & HID_MOD_CTRL) != 0) {
        const char control = hid_keycode_to_control_char(keycode);
        if (control != '\0') {
            return control;
        }
    }

    if (keyboard_layout == SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE) {
        return hid_keycode_to_char_de(keycode, modifiers);
    }

    return hid_keycode_to_char_us(keycode, shift);
}

static bool hid_should_prefix_alt(uint8_t modifiers, uint8_t keycode, char ch)
{
    const bool left_alt = (modifiers & HID_MOD_LEFT_ALT) != 0;

    return left_alt &&
        ch != '\0' &&
        (uint8_t)ch != SOLAR_OS_KEY_APP_EXIT;
}

static bool hid_is_alt_tab(uint8_t modifiers, uint8_t keycode)
{
    return (modifiers & HID_MOD_ALT) != 0 && keycode == 0x2b;
}

static void handle_keyboard_report(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length < 8) {
        return;
    }

    const uint8_t modifiers = data[0];
    const uint8_t *keys = &data[2];

    if (keys[0] == 0x01) {
        memset(previous_keys, 0, sizeof(previous_keys));
        previous_modifiers = modifiers;
        repeat_clear();
        return;
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        const uint8_t key = keys[i];
        if (key == 0 || key_in_report(key, previous_keys)) {
            continue;
        }

        if (key == 0x39) {
            caps_lock = !caps_lock;
            continue;
        }

        if (hid_is_alt_tab(modifiers, key)) {
            queue_char((char)SOLAR_OS_KEY_ALT_PREFIX);
            queue_char('\t');
            repeat_clear();
            continue;
        }

        const char ch = hid_keycode_to_char(key, modifiers);
        if (ch != '\0') {
            char sequence[BLE_KEYBOARD_REPEAT_SEQUENCE_MAX];
            size_t sequence_len = 0;

            if (hid_should_prefix_alt(modifiers, key, ch)) {
                queue_char((char)SOLAR_OS_KEY_ALT_PREFIX);
                sequence[sequence_len++] = (char)SOLAR_OS_KEY_ALT_PREFIX;
            }
            queue_char(ch);
            sequence[sequence_len++] = ch;

            if (repeatable_char(ch)) {
                repeat_start(key, sequence, sequence_len);
            } else {
                repeat_clear();
            }
        }
    }

    repeat_stop_if_released(keys);
    memcpy(previous_keys, keys, sizeof(previous_keys));
    previous_modifiers = modifiers;
}

static void gap_callback(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        xSemaphoreGive(scan_done_sem);
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        switch (param->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT:
            consider_candidate(param);
            break;

        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
            SOLAR_OS_LOGI(TAG, "scan complete: %d responses", param->scan_rst.num_resps);
            xSemaphoreGive(scan_done_sem);
            break;

        default:
            break;
        }
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        SOLAR_OS_LOGI(TAG, "security request");
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        SOLAR_OS_LOGI(TAG, "type passkey %" PRIu32 " on the keyboard, then Enter",
                 param->ble_security.key_notif.passkey);
        set_status(BLE_KEYBOARD_PASSKEY,
                   "type %" PRIu32 " Enter",
                   param->ble_security.key_notif.passkey);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        SOLAR_OS_LOGI(TAG, "numeric comparison passkey %" PRIu32 ": accepting",
                 param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        SOLAR_OS_LOGW(TAG, "peer requested a passkey; no numeric input is available");
        set_status(BLE_KEYBOARD_FAILED, "passkey input needed");
        break;

    case ESP_GAP_BLE_KEY_EVT:
        SOLAR_OS_LOGI(TAG, "key exchanged: %s", key_type_name(param->ble_security.ble_key.key_type));
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            SOLAR_OS_LOGI(TAG, "auth success");
        } else {
            SOLAR_OS_LOGE(TAG, "auth failed: 0x%x", param->ble_security.auth_cmpl.fail_reason);
            set_status(BLE_KEYBOARD_FAILED, "auth failed 0x%x", param->ble_security.auth_cmpl.fail_reason);
        }
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        SOLAR_OS_LOGI(TAG,
                 "conn params update status=%d interval=%u ms latency=%u timeout=%u ms",
                 param->update_conn_params.status,
                 (unsigned)param->update_conn_params.conn_int,
                 (unsigned)param->update_conn_params.latency,
                 (unsigned)param->update_conn_params.timeout * 10U);
        break;

    default:
        break;
    }
}

static bool gatt_event_is_for_hid(esp_gattc_cb_event_t event,
                                  esp_gatt_if_t gattc_if,
                                  const esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        return param != NULL && param->reg.app_id == 0;
    }
    return hid_gattc_if != ESP_GATT_IF_NONE &&
        (gattc_if == hid_gattc_if || gattc_if == ESP_GATT_IF_NONE);
}

static bool gatt_event_is_for_solaros(esp_gattc_cb_event_t event,
                                      esp_gatt_if_t gattc_if,
                                      const esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_REG_EVT) {
        return param != NULL && param->reg.app_id == BLE_GATT_APP_ID;
    }
    return gatt_state.gattc_if != ESP_GATT_IF_NONE &&
        gattc_if == gatt_state.gattc_if;
}

static void gatt_handle_disconnect(uint16_t conn_id, const uint8_t *bda, uint8_t reason)
{
    bool active = false;

    gatt_lock();
    active = gatt_state.connected &&
        (gatt_state.conn_id == conn_id ||
         (bda != NULL && memcmp(gatt_state.bda, bda, sizeof(gatt_state.bda)) == 0));
    if (active) {
        gatt_state.connected = false;
        gatt_state.connecting = false;
        gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
        gatt_state.mtu = 0;
        gatt_clear_services_locked();
        gatt_set_status_locked("disconnected 0x%02x", reason);
    }
    gatt_unlock();

    if (active) {
        gatt_complete_operation(BLE_GATT_OP_NONE, ESP_GATT_ERROR);
    }
}

static void solaros_gattc_event_handler(esp_gattc_cb_event_t event,
                                        esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    if (param == NULL) {
        return;
    }

    switch (event) {
    case ESP_GATTC_REG_EVT:
        gatt_lock();
        if (param->reg.status == ESP_GATT_OK) {
            gatt_state.gattc_if = gattc_if;
            gatt_state.registered = true;
            gatt_set_status_locked("idle");
        } else {
            gatt_state.gattc_if = ESP_GATT_IF_NONE;
            gatt_state.registered = false;
            gatt_set_status_locked("register failed 0x%02x", param->reg.status);
        }
        gatt_unlock();
        if (gatt_op_sem != NULL) {
            xSemaphoreGive(gatt_op_sem);
        }
        break;

    case ESP_GATTC_OPEN_EVT:
        gatt_lock();
        if (param->open.status != ESP_GATT_OK) {
            gatt_state.connecting = false;
            gatt_state.connected = false;
            gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
            gatt_set_status_locked("open failed 0x%02x", param->open.status);
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, param->open.status);
            break;
        }

        gatt_state.connected = true;
        gatt_state.connecting = false;
        gatt_state.conn_id = param->open.conn_id;
        gatt_state.mtu = param->open.mtu;
        memcpy(gatt_state.bda, param->open.remote_bda, sizeof(gatt_state.bda));
        gatt_clear_services_locked();
        gatt_set_status_locked("discovering");
        gatt_unlock();

        (void)esp_ble_gattc_send_mtu_req(gattc_if, param->open.conn_id);
        if (esp_ble_gattc_search_service(gattc_if, param->open.conn_id, NULL) != ESP_OK) {
            gatt_lock();
            gatt_set_status_locked("service discovery failed");
            gatt_unlock();
            gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_GATT_ERROR);
        }
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == param->cfg_mtu.conn_id &&
            param->cfg_mtu.status == ESP_GATT_OK) {
            gatt_state.mtu = param->cfg_mtu.mtu;
        }
        gatt_unlock();
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        gatt_lock();
        if (gatt_state.connected &&
            gatt_state.conn_id == param->search_res.conn_id &&
            gatt_state.service_count < SOLAR_OS_BLE_GATT_MAX_SERVICES) {
            solar_os_ble_gatt_service_t *service =
                &gatt_state.services[gatt_state.service_count++];
            service->start_handle = param->search_res.start_handle;
            service->end_handle = param->search_res.end_handle;
            service->primary = param->search_res.is_primary;
            gatt_uuid_to_string(&param->search_res.srvc_id.uuid,
                                service->uuid,
                                sizeof(service->uuid));
        }
        gatt_unlock();
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == param->search_cmpl.conn_id) {
            if (param->search_cmpl.status == ESP_GATT_OK) {
                gatt_set_status_locked("connected");
            } else {
                gatt_set_status_locked("search failed 0x%02x", param->search_cmpl.status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, param->search_cmpl.status);
        break;

    case ESP_GATTC_READ_CHAR_EVT:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == param->read.conn_id &&
            gatt_state.op == BLE_GATT_OP_READ) {
            gatt_state.op_status = param->read.status;
            gatt_state.op_value_len = 0;
            if (param->read.status == ESP_GATT_OK && param->read.value != NULL) {
                const size_t copy_len =
                    param->read.value_len < SOLAR_OS_BLE_GATT_VALUE_MAX ?
                    param->read.value_len :
                    SOLAR_OS_BLE_GATT_VALUE_MAX;
                memcpy(gatt_state.op_value, param->read.value, copy_len);
                gatt_state.op_value_len = copy_len;
                gatt_set_status_locked("read handle 0x%04x", param->read.handle);
            } else {
                gatt_set_status_locked("read failed 0x%02x", param->read.status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_READ, param->read.status);
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        gatt_lock();
        if (gatt_state.connected && gatt_state.conn_id == param->write.conn_id &&
            gatt_state.op == BLE_GATT_OP_WRITE) {
            gatt_state.op_status = param->write.status;
            if (param->write.status == ESP_GATT_OK) {
                gatt_set_status_locked("wrote handle 0x%04x", param->write.handle);
            } else {
                gatt_set_status_locked("write failed 0x%02x", param->write.status);
            }
        }
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_WRITE, param->write.status);
        break;

    case ESP_GATTC_CLOSE_EVT:
        gatt_handle_disconnect(param->close.conn_id, param->close.remote_bda, (uint8_t)param->close.reason);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        gatt_handle_disconnect(param->disconnect.conn_id,
                               param->disconnect.remote_bda,
                               (uint8_t)param->disconnect.reason);
        break;

    default:
        break;
    }
}

static void ble_gattc_callback(esp_gattc_cb_event_t event,
                               esp_gatt_if_t gattc_if,
                               esp_ble_gattc_cb_param_t *param)
{
    if (gatt_event_is_for_hid(event, gattc_if, param)) {
        if (event == ESP_GATTC_REG_EVT && param != NULL && param->reg.status == ESP_GATT_OK) {
            hid_gattc_if = gattc_if;
        }
        esp_hidh_gattc_event_handler(event, gattc_if, param);
    }

    if (gatt_event_is_for_solaros(event, gattc_if, param)) {
        solaros_gattc_event_handler(event, gattc_if, param);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (pending_dev == param->open.dev) {
            pending_dev = NULL;
        }
        pending_open_started_tick = 0;
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            esp_gap_conn_params_t params = {0};
            if (bda == NULL || !hidh_link_ready(param->open.dev, &params)) {
                connected = false;
                connected_dev = NULL;
                pending_dev = NULL;
                memset(previous_keys, 0, sizeof(previous_keys));
                previous_modifiers = 0;
                repeat_clear();
                SOLAR_OS_LOGW(TAG, "open rejected: HID keyboard link is not usable");
                set_status(BLE_KEYBOARD_FAILED, "bad keyboard link");
                const esp_err_t close_ret = esp_hidh_dev_close(param->open.dev);
                if (close_ret != ESP_OK) {
                    SOLAR_OS_LOGW(TAG,
                                  "bad keyboard link close failed: %s",
                                  esp_err_to_name(close_ret));
                    if (!reconnect_is_suppressed()) {
                        schedule_reconnect(BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS);
                    }
                }
                break;
            }

            connected = true;
            connected_dev = param->open.dev;
            memset(previous_keys, 0, sizeof(previous_keys));
            previous_modifiers = 0;
            repeat_clear();
            const char *name = esp_hidh_dev_name_get(param->open.dev);
            const char *display_name = name != NULL && name[0] ? name : pending_name;
            strlcpy(connected_name, display_name[0] ? display_name : "keyboard", sizeof(connected_name));
            SOLAR_OS_LOGI(TAG, ESP_BD_ADDR_STR " open: %s",
                     ESP_BD_ADDR_HEX(bda),
                     connected_name);
            log_conn_params("open", &params);
            save_remembered_peer(bda, pending_addr_type, connected_name);
            esp_hidh_dev_dump(param->open.dev, stdout);
            set_status(BLE_KEYBOARD_CONNECTED, "connected %s", connected_name);
            if (pairing_retry_pending) {
                esp_err_t pair_ret = start_pairing_scan_now();
                if (pair_ret != ESP_OK) {
                    SOLAR_OS_LOGW(TAG,
                                  "deferred pairing start failed after open: %s",
                                  esp_err_to_name(pair_ret));
                }
            }
        } else {
            connected = false;
            connected_dev = NULL;
            pending_dev = NULL;
            memset(previous_keys, 0, sizeof(previous_keys));
            previous_modifiers = 0;
            SOLAR_OS_LOGE(TAG, "open failed: %s", esp_err_to_name(param->open.status));
            if (pairing_retry_pending) {
                esp_err_t pair_ret = start_pairing_scan_now();
                if (pair_ret != ESP_OK) {
                    SOLAR_OS_LOGW(TAG,
                                  "deferred pairing start failed after open failure: %s",
                                  esp_err_to_name(pair_ret));
                    set_status(BLE_KEYBOARD_FAILED, "open failed");
                }
                break;
            }
            set_status(BLE_KEYBOARD_FAILED, "open failed");
            if (!reconnect_is_suppressed()) {
                schedule_reconnect(BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS);
            }
        }
        break;

    case ESP_HIDH_BATTERY_EVENT:
        SOLAR_OS_LOGI(TAG, "battery %u%%", param->battery.level);
        break;

    case ESP_HIDH_INPUT_EVENT:
        SOLAR_OS_LOGD(TAG,
                      "input usage=%s map=%u report=%u len=%u",
                      esp_hid_usage_str(param->input.usage),
                      param->input.map_index,
                      param->input.report_id,
                      param->input.length);
        solar_os_log_buffer_hex(SOLAR_OS_LOG_LEVEL_DEBUG,
                                TAG,
                                param->input.data,
                                param->input.length);
        if (param->input.usage == ESP_HID_USAGE_KEYBOARD) {
            handle_keyboard_report(param->input.data, param->input.length);
            set_status(BLE_KEYBOARD_CONNECTED, "connected %s", connected_name[0] ? connected_name : "keyboard");
        }
        break;

    case ESP_HIDH_CLOSE_EVENT:
        pending_open_started_tick = 0;
        connected = false;
        if (connected_dev == param->close.dev) {
            connected_dev = NULL;
        }
        if (pending_dev == param->close.dev) {
            pending_dev = NULL;
        }
        memset(previous_keys, 0, sizeof(previous_keys));
        previous_modifiers = 0;
        repeat_clear();
        SOLAR_OS_LOGI(TAG, "close reason=%d status=%s",
                 param->close.reason,
                 esp_err_to_name(param->close.status));
        esp_hidh_dev_free(param->close.dev);
        set_status(BLE_KEYBOARD_IDLE, "disconnected");
        if (close_done_sem != NULL) {
            xSemaphoreGive(close_done_sem);
        }
        if (pairing_retry_pending) {
            esp_err_t pair_ret = start_pairing_scan_now();
            if (pair_ret != ESP_OK) {
                SOLAR_OS_LOGW(TAG,
                              "deferred pairing start failed after close: %s",
                              esp_err_to_name(pair_ret));
            }
        } else if (!reconnect_is_suppressed()) {
            schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
        }
        break;

    default:
        SOLAR_OS_LOGI(TAG, "event %" PRIi32, id);
        break;
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
        if (ret == ESP_ERR_INVALID_STATE) {
            return ESP_OK;
        }
    }
    return ret;
}

static esp_err_t init_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t oob_support = ESP_BLE_OOB_DISABLE;

    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req)),
                        TAG, "set auth req failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap)),
                        TAG, "set io cap failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size)),
                        TAG, "set key size failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_OOB_SUPPORT, &oob_support, sizeof(oob_support)),
                        TAG, "set oob support failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key)),
                        TAG, "set init key failed");
    ESP_RETURN_ON_ERROR(esp_ble_gap_set_security_param(
                            ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key)),
                        TAG, "set rsp key failed");

    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ensure_runtime_objects(), TAG, "runtime object setup failed");
    if (char_queue != NULL) {
        xQueueReset(char_queue);
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    esp_err_t layout_ret = load_keyboard_layout();
    if (layout_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load keyboard layout failed: %s", esp_err_to_name(layout_ret));
    }
    esp_err_t repeat_ret = load_keyboard_repeat();
    if (repeat_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load keyboard repeat failed: %s", esp_err_to_name(repeat_ret));
    }
    esp_err_t peer_ret = load_remembered_peers();
    if (peer_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load remembered keyboard failed: %s", esp_err_to_name(peer_ret));
    }

    esp_err_t ret = ESP_OK;
    if (!classic_bt_memory_released) {
        ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            classic_bt_memory_released = true;
        } else {
            SOLAR_OS_LOGW(TAG, "classic bt memory release failed: %s", esp_err_to_name(ret));
        }
    }

    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller init failed");
        controller_status = esp_bt_controller_get_status();
    }
    if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE),
                            TAG,
                            "controller enable failed");
    } else if (controller_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        SOLAR_OS_LOGW(TAG, "unexpected controller status %d", (int)controller_status);
        return ESP_ERR_INVALID_STATE;
    }
    (void)solar_os_power_apply_runtime_policy();

    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
        bluedroid_cfg.ssp_en = false;
        ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_cfg),
                            TAG,
                            "bluedroid init failed");
        bluedroid_status = esp_bluedroid_get_status();
    }
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable failed");
    } else if (bluedroid_status != ESP_BLUEDROID_STATUS_ENABLED) {
        SOLAR_OS_LOGW(TAG, "unexpected bluedroid status %d", (int)bluedroid_status);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_callback), TAG, "gap callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(ble_gattc_callback),
                        TAG, "gattc callback failed");
    ESP_RETURN_ON_ERROR(init_security(), TAG, "security init failed");

    esp_hidh_config_t hidh_config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    if (!hidh_initialized) {
        ESP_RETURN_ON_ERROR(esp_hidh_init(&hidh_config), TAG, "hid host init failed");
        hidh_initialized = true;
    }

    gatt_drain_op_sem();
    reset_gatt_runtime_state("idle");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_app_register(BLE_GATT_APP_ID),
                        TAG,
                        "generic gatt app register failed");
    if (xSemaphoreTake(gatt_op_sem, pdMS_TO_TICKS(BLE_GATT_OPERATION_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (!gatt_state.registered) {
        return ESP_FAIL;
    }

    initialized = true;
    set_status(BLE_KEYBOARD_IDLE, "idle");
    if (remembered_peer_count() > 0) {
        start_fast_reconnect_window("boot");
        schedule_reconnect(0);
    }
    SOLAR_OS_LOGI(TAG, "BLE keyboard host ready");
    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results,
                                     size_t *found)
{
    if (found != NULL) {
        *found = 0;
    }
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (results == NULL || max_results == 0 || found == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (scan_task_handle != NULL ||
        reconnect_task_handle != NULL ||
        state == BLE_KEYBOARD_SCANNING ||
        state == BLE_KEYBOARD_CONNECTING ||
        state == BLE_KEYBOARD_PASSKEY) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(results, 0, max_results * sizeof(results[0]));
    active_scan_results = results;
    active_scan_max_results = max_results;
    active_scan_result_count = 0;
    collect_connected_scan_result();

    const esp_err_t ret = run_keyboard_scan(BLE_KEYBOARD_SCAN_DISCOVERY);

    *found = active_scan_result_count;
    active_scan_results = NULL;
    active_scan_max_results = 0;
    active_scan_result_count = 0;
    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;

    if (ret == ESP_ERR_NOT_FOUND && *found > 0) {
        return ESP_OK;
    }
    if (ret == ESP_OK && !connected) {
        set_status(BLE_KEYBOARD_IDLE, "scan done");
    } else if (ret == ESP_OK) {
        restore_status_after_scan(BLE_KEYBOARD_SCAN_DISCOVERY);
    }
    return ret;
}

esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms)
{
    if (bda == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_ble_keyboard_init();
    if (ret != ESP_OK) {
        return ret;
    }

    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;
    gatt_lock();
    if (!gatt_state.registered || gatt_state.gattc_if == ESP_GATT_IF_NONE) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (gatt_state.connected || gatt_state.connecting) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    gattc_if = gatt_state.gattc_if;
    gatt_unlock();

    ret = gatt_begin_operation(BLE_GATT_OP_CONNECT);
    if (ret != ESP_OK) {
        return ret;
    }

    esp_ble_gatt_creat_conn_params_t params = {0};
    memcpy(params.remote_bda, bda, ESP_BD_ADDR_LEN);
    params.remote_addr_type = (esp_ble_addr_type_t)addr_type;
    params.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    params.is_direct = true;
    params.is_aux = false;
    params.phy_mask = 0x0;

    gatt_lock();
    gatt_state.connecting = true;
    gatt_state.connected = false;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.addr_type = (esp_ble_addr_type_t)addr_type;
    memcpy(gatt_state.bda, bda, sizeof(gatt_state.bda));
    gatt_clear_services_locked();
    gatt_set_status_locked("connecting");
    gatt_unlock();

    ret = esp_ble_gattc_enh_open(gattc_if, &params);
    if (ret != ESP_OK) {
        gatt_lock();
        gatt_state.connecting = false;
        gatt_set_status_locked("connect failed %s", esp_err_to_name(ret));
        gatt_unlock();
        gatt_complete_operation(BLE_GATT_OP_CONNECT, ESP_GATT_ERROR);
        return ret;
    }

    return gatt_wait_operation(BLE_GATT_OP_CONNECT,
                               timeout_ms != 0 ? timeout_ms : BLE_GATT_CONNECT_TIMEOUT_MS);
}

esp_err_t solar_os_ble_gatt_disconnect(void)
{
    esp_err_t ret = solar_os_ble_keyboard_init();
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;
    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_state.connecting = false;
        gatt_set_status_locked("idle");
        gatt_unlock();
        return ESP_OK;
    }
    conn_id = gatt_state.conn_id;
    gattc_if = gatt_state.gattc_if;
    gatt_unlock();

    ret = esp_ble_gattc_close(gattc_if, conn_id);
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    gatt_state.connected = false;
    gatt_state.connecting = false;
    gatt_state.conn_id = BLE_GATT_INVALID_CONN_ID;
    gatt_state.mtu = 0;
    gatt_clear_services_locked();
    gatt_set_status_locked("disconnected");
    gatt_unlock();
    return ESP_OK;
}

void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status)
{
    if (status == NULL) {
        return;
    }

    gatt_lock();
    *status = (solar_os_ble_gatt_status_t){
        .connected = gatt_state.connected,
        .addr_type = (uint8_t)gatt_state.addr_type,
        .conn_id = gatt_state.conn_id,
        .mtu = gatt_state.mtu,
        .service_count = gatt_state.service_count,
    };
    memcpy(status->bda, gatt_state.bda, sizeof(status->bda));
    strlcpy(status->status, gatt_state.status, sizeof(status->status));
    gatt_unlock();
}

esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_services > 0 && services == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const size_t copy_count =
        gatt_state.service_count < max_services ? gatt_state.service_count : max_services;
    if (copy_count > 0) {
        memcpy(services, gatt_state.services, copy_count * sizeof(services[0]));
    }
    if (count != NULL) {
        *count = gatt_state.service_count;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_characteristics > 0 && characteristics == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_ble_gatt_service_t service = {0};
    uint16_t conn_id = BLE_GATT_INVALID_CONN_ID;
    esp_gatt_if_t gattc_if = ESP_GATT_IF_NONE;

    gatt_lock();
    if (!gatt_state.connected) {
        gatt_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (service_index >= gatt_state.service_count) {
        gatt_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    service = gatt_state.services[service_index];
    conn_id = gatt_state.conn_id;
    gattc_if = gatt_state.gattc_if;
    gatt_unlock();

    esp_gattc_char_elem_t chars[SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS] = {0};
    uint16_t char_count = max_characteristics > SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS ?
        SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS :
        (uint16_t)max_characteristics;
    if (char_count == 0) {
        return ESP_OK;
    }

    esp_gatt_status_t status = esp_ble_gattc_get_all_char(gattc_if,
                                                          conn_id,
                                                          service.start_handle,
                                                          service.end_handle,
                                                          chars,
                                                          &char_count,
                                                          0);
    if (status != ESP_GATT_OK) {
        return ESP_FAIL;
    }

    for (uint16_t i = 0; i < char_count; i++) {
        characteristics[i].handle = chars[i].char_handle;
        characteristics[i].properties = chars[i].properties;
        gatt_uuid_to_string(&chars[i].uuid,
                            characteristics[i].uuid,
                            sizeof(characteristics[i].uuid));
    }
    if (count != NULL) {
        *count = char_count;
    }
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms)
{
    if (value_len != NULL) {
        *value_len = 0;
    }
    if (handle == 0 || (max_len > 0 && value == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    const bool can_read = gatt_state.connected && gatt_state.gattc_if != ESP_GATT_IF_NONE;
    const uint16_t conn_id = gatt_state.conn_id;
    const esp_gatt_if_t gattc_if = gatt_state.gattc_if;
    gatt_unlock();
    if (!can_read) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_READ);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_ble_gattc_read_char(gattc_if, conn_id, handle, ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        gatt_complete_operation(BLE_GATT_OP_READ, ESP_GATT_ERROR);
        return ret;
    }

    ret = gatt_wait_operation(BLE_GATT_OP_READ,
                              timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
    if (ret != ESP_OK) {
        return ret;
    }

    gatt_lock();
    const size_t copy_len = gatt_state.op_value_len < max_len ? gatt_state.op_value_len : max_len;
    if (copy_len > 0) {
        memcpy(value, gatt_state.op_value, copy_len);
    }
    if (value_len != NULL) {
        *value_len = gatt_state.op_value_len;
    }
    gatt_unlock();
    return ESP_OK;
}

esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t value_len,
                                  bool with_response,
                                  uint32_t timeout_ms)
{
    if (handle == 0 || value == NULL || value_len == 0 || value_len > SOLAR_OS_BLE_GATT_VALUE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    gatt_lock();
    const bool can_write = gatt_state.connected && gatt_state.gattc_if != ESP_GATT_IF_NONE;
    const uint16_t conn_id = gatt_state.conn_id;
    const esp_gatt_if_t gattc_if = gatt_state.gattc_if;
    gatt_unlock();
    if (!can_write) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = gatt_begin_operation(BLE_GATT_OP_WRITE);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t buffer[SOLAR_OS_BLE_GATT_VALUE_MAX];
    memcpy(buffer, value, value_len);
    ret = esp_ble_gattc_write_char(gattc_if,
                                   conn_id,
                                   handle,
                                   (uint16_t)value_len,
                                   buffer,
                                   with_response ? ESP_GATT_WRITE_TYPE_RSP : ESP_GATT_WRITE_TYPE_NO_RSP,
                                   ESP_GATT_AUTH_REQ_NONE);
    if (ret != ESP_OK) {
        gatt_complete_operation(BLE_GATT_OP_WRITE, ESP_GATT_ERROR);
        return ret;
    }

    if (!with_response) {
        gatt_complete_operation(BLE_GATT_OP_WRITE, ESP_GATT_OK);
        return ESP_OK;
    }

    return gatt_wait_operation(BLE_GATT_OP_WRITE,
                               timeout_ms != 0 ? timeout_ms : BLE_GATT_OPERATION_TIMEOUT_MS);
}

static const char *scan_mode_status(ble_keyboard_scan_mode_t mode)
{
    switch (mode) {
    case BLE_KEYBOARD_SCAN_PAIRING:
        return "pairing";
    case BLE_KEYBOARD_SCAN_DISCOVERY:
    default:
        return "scanning";
    }
}

static const char *scan_mode_log_name(ble_keyboard_scan_mode_t mode)
{
    switch (mode) {
    case BLE_KEYBOARD_SCAN_PAIRING:
        return "new keyboard pairing";
    case BLE_KEYBOARD_SCAN_DISCOVERY:
    default:
        return "BLE discovery";
    }
}

static void restore_status_after_scan(ble_keyboard_scan_mode_t mode)
{
    if (connected) {
        set_status(BLE_KEYBOARD_CONNECTED,
                   "connected %s",
                   connected_name[0] ? connected_name : "keyboard");
    } else {
        const char *message = "no keyboard found";
        if (mode == BLE_KEYBOARD_SCAN_PAIRING) {
            message = "no new keyboard found";
        }
        set_status(BLE_KEYBOARD_IDLE, "%s", message);
    }
}

static esp_err_t run_keyboard_scan(ble_keyboard_scan_mode_t mode)
{
    while (xSemaphoreTake(scan_done_sem, 0) == pdTRUE) {
    }

    memset(&candidate, 0, sizeof(candidate));
    active_scan_mode = mode;
    set_status(BLE_KEYBOARD_SCANNING, "%s", scan_mode_status(mode));
    SOLAR_OS_LOGI(TAG, "%s scan start", scan_mode_log_name(mode));

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "set scan params failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "scan setup failed");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ret;
    }

    if (xSemaphoreTake(scan_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        SOLAR_OS_LOGE(TAG, "scan params timeout");
        set_status(BLE_KEYBOARD_FAILED, "scan setup timeout");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ESP_ERR_TIMEOUT;
    }

    ret = esp_ble_gap_start_scanning(BLE_KEYBOARD_SCAN_SECONDS);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "scan start failed");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ret;
    }

    if (xSemaphoreTake(scan_done_sem, pdMS_TO_TICKS((BLE_KEYBOARD_SCAN_SECONDS + 2) * 1000)) != pdTRUE) {
        SOLAR_OS_LOGE(TAG, "scan timeout");
        esp_ble_gap_stop_scanning();
        set_status(BLE_KEYBOARD_FAILED, "scan timeout");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ESP_ERR_TIMEOUT;
    }
    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;

    if (!candidate.valid) {
        SOLAR_OS_LOGW(TAG,
                      "%s candidate not found",
                      scan_mode_log_name(mode));
        restore_status_after_scan(mode);
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static esp_err_t close_connected_keyboard_for_pairing(void)
{
    if (!connected || connected_dev == NULL) {
        return ESP_OK;
    }

    while (close_done_sem != NULL && xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
    }

    SOLAR_OS_LOGI(TAG, "pairing: closing connected keyboard before switch");
    reconnect_suppressed_for_pairing = true;
    esp_hidh_dev_close(connected_dev);

    esp_err_t ret = ESP_OK;
    if (close_done_sem != NULL &&
        xSemaphoreTake(close_done_sem,
                       pdMS_TO_TICKS(BLE_KEYBOARD_PAIR_SWITCH_DISCONNECT_TIMEOUT_MS)) != pdTRUE) {
        SOLAR_OS_LOGW(TAG, "pairing: keyboard disconnect timeout");
        clear_runtime_connection_state("pairing disconnect timeout");
        ret = ESP_ERR_TIMEOUT;
    }

    reconnect_suppressed_for_pairing = false;
    return ret;
}

static esp_err_t scan_and_open_keyboard(ble_keyboard_scan_mode_t mode)
{
    esp_err_t ret = run_keyboard_scan(mode);
    if (ret != ESP_OK) {
        return ret;
    }
    if (mode == BLE_KEYBOARD_SCAN_PAIRING && pairing_cancel_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    SOLAR_OS_LOGI(TAG,
             "connecting " ESP_BD_ADDR_STR " addr_type=%s name=%s",
             ESP_BD_ADDR_HEX(candidate.bda),
             addr_type_name(candidate.addr_type),
             candidate.name[0] ? candidate.name : "(none)");
    set_status(BLE_KEYBOARD_CONNECTING,
               "connecting %s",
               candidate.name[0] ? candidate.name : "keyboard");

    if (mode == BLE_KEYBOARD_SCAN_PAIRING) {
        ret = close_connected_keyboard_for_pairing();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return open_keyboard(candidate.bda,
                         candidate.addr_type,
                         candidate.name,
                         mode == BLE_KEYBOARD_SCAN_PAIRING ? "pairing" : "connecting");
}

static void scan_task(void *arg)
{
    (void)arg;

    const esp_err_t ret = scan_and_open_keyboard(BLE_KEYBOARD_SCAN_PAIRING);
    if (pairing_cancel_requested) {
        pairing_cancel_requested = false;
        if (!connected) {
            set_status(BLE_KEYBOARD_IDLE, "pairing cancelled");
        }
    }
    if (ret != ESP_OK && !connected) {
        schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
    }

    scan_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_pairing_scan_now(void)
{
    pairing_retry_pending = false;
    reconnect_suppressed_for_pairing = false;

    if (state == BLE_KEYBOARD_CONNECTING && pending_dev == NULL && !connected) {
        set_status(BLE_KEYBOARD_IDLE, "pairing");
    }

    if (scan_task_handle != NULL ||
        state == BLE_KEYBOARD_SCANNING ||
        state == BLE_KEYBOARD_CONNECTING ||
        state == BLE_KEYBOARD_PASSKEY) {
        return ESP_ERR_INVALID_STATE;
    }

    set_status(BLE_KEYBOARD_SCANNING, "pairing");
    if (xTaskCreate(scan_task, "ble_kbd_scan", 6144, NULL, 4, &scan_task_handle) != pdPASS) {
        scan_task_handle = NULL;
        set_status(BLE_KEYBOARD_FAILED, "scan task failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void request_pairing_after_pending_connect(const char *reason)
{
    pairing_retry_pending = true;
    reconnect_suppressed_for_pairing = false;
    set_status(BLE_KEYBOARD_PAIRING_PENDING, "pairing pending");
    SOLAR_OS_LOGI(TAG,
                  "%s: pairing waits for pending HID open",
                  reason != NULL ? reason : "pairing");
}

static bool pending_open_timed_out(void)
{
    return pending_dev != NULL &&
        pending_open_started_tick != 0 &&
        (int32_t)(xTaskGetTickCount() -
                  (pending_open_started_tick + pdMS_TO_TICKS(BLE_KEYBOARD_OPEN_TIMEOUT_MS))) >= 0;
}

static void reconnect_task(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;

    if (delay_ms > 0) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    while (initialized && remembered_peer_count() > 0 && !connected) {
        if (pending_open_timed_out()) {
            SOLAR_OS_LOGW(TAG, "keyboard open timeout, retrying remembered keyboard");
            (void)close_pending_open_attempt("open timeout", 0);
            if (!connected && state == BLE_KEYBOARD_CONNECTING) {
                set_status(BLE_KEYBOARD_FAILED, "open timeout");
            }
        }

        if (scan_task_handle == NULL &&
            state != BLE_KEYBOARD_SCANNING &&
            state != BLE_KEYBOARD_CONNECTING &&
            state != BLE_KEYBOARD_PASSKEY) {
            const ble_keyboard_peer_t *peer = primary_remembered_peer();
            if (peer == NULL) {
                break;
            }
            SOLAR_OS_LOGI(TAG,
                          "reconnecting remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
                          ESP_BD_ADDR_HEX(peer->bda),
                          addr_type_name((esp_ble_addr_type_t)peer->addr_type),
                          peer->name[0] ? peer->name : "(unnamed)");
            const esp_err_t ret = open_keyboard(peer->bda,
                                                (esp_ble_addr_type_t)peer->addr_type,
                                                peer->name,
                                                "reconnecting");
            if (ret == ESP_OK) {
                SOLAR_OS_LOGI(TAG, "reconnect attempt started");
            }
        }

        const uint32_t retry_delay_ms = reconnect_fast_active() ?
            BLE_KEYBOARD_RECONNECT_FAST_RETRY_DELAY_MS :
            BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS;
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retry_delay_ms));
    }

    reconnect_task_handle = NULL;
    vTaskDelete(NULL);
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (!initialized || connected || remembered_peer_count() == 0 || reconnect_is_suppressed()) {
        return;
    }
    if (reconnect_task_handle != NULL) {
        xTaskNotifyGive(reconnect_task_handle);
        return;
    }

    if (xTaskCreate(reconnect_task,
                    "ble_kbd_reconn",
                    4096,
                    (void *)(uintptr_t)delay_ms,
                    3,
                    &reconnect_task_handle) != pdPASS) {
        reconnect_task_handle = NULL;
        SOLAR_OS_LOGW(TAG, "reconnect task failed");
    }
}

esp_err_t solar_os_ble_keyboard_start_pairing(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    pairing_cancel_requested = false;
    stop_reconnect_task("pairing");

    reconnect_suppressed_for_pairing = true;
    const bool pending_closed =
        close_pending_open_attempt("pairing", BLE_KEYBOARD_PAIR_SWITCH_DISCONNECT_TIMEOUT_MS);
    reconnect_suppressed_for_pairing = false;
    if (!pending_closed) {
        request_pairing_after_pending_connect("pairing");
        return ESP_OK;
    }

    if (state == BLE_KEYBOARD_CONNECTING) {
        request_pairing_after_pending_connect("pairing");
        return ESP_OK;
    }

    if (state == BLE_KEYBOARD_PAIRING_PENDING) {
        return ESP_OK;
    }

    if (scan_task_handle != NULL ||
        state == BLE_KEYBOARD_SCANNING ||
        state == BLE_KEYBOARD_PASSKEY) {
        return ESP_ERR_INVALID_STATE;
    }

    return start_pairing_scan_now();
}

esp_err_t solar_os_ble_keyboard_cancel_pairing(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const bool was_pairing = solar_os_ble_keyboard_is_pairing();
    pairing_retry_pending = false;
    pairing_cancel_requested = false;
    reconnect_suppressed_for_pairing = false;

    if (state == BLE_KEYBOARD_SCANNING && active_scan_mode == BLE_KEYBOARD_SCAN_PAIRING) {
        pairing_cancel_requested = true;
        SOLAR_OS_LOGI(TAG, "pairing cancel: stopping scan");
        (void)esp_ble_gap_stop_scanning();
    }

    if (pending_dev != NULL && state == BLE_KEYBOARD_CONNECTING) {
        (void)close_pending_open_attempt("pairing cancel", 0);
    }

    if (!connected &&
        (state == BLE_KEYBOARD_PAIRING_PENDING ||
         state == BLE_KEYBOARD_CONNECTING ||
         state == BLE_KEYBOARD_FAILED ||
         state == BLE_KEYBOARD_IDLE ||
         !was_pairing)) {
        set_status(BLE_KEYBOARD_IDLE, was_pairing ? "pairing cancelled" : "idle");
    }

    if (!connected && scan_task_handle == NULL) {
        schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
    }

    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms)
{
    if (!initialized) {
        return ESP_OK;
    }

    esp_err_t result = ESP_OK;

    reconnect_suppressed_for_sleep = true;
    reconnect_fast_until_tick = 0;
    pairing_retry_pending = false;
    reconnect_suppressed_for_pairing = false;

    stop_reconnect_task("sleep");
    stop_scan_task_for_sleep(timeout_ms);

    while (close_done_sem != NULL && xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
    }

    if (!close_pending_open_attempt("sleep", timeout_ms)) {
        result = ESP_ERR_TIMEOUT;
    }

    if (connected && connected_dev != NULL) {
        SOLAR_OS_LOGI(TAG, "sleep: closing keyboard connection");
        esp_hidh_dev_t *dev = connected_dev;
        esp_hidh_dev_close(dev);

        if (close_done_sem != NULL && timeout_ms > 0) {
            if (xSemaphoreTake(close_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
                SOLAR_OS_LOGI(TAG, "sleep: keyboard disconnected");
            } else {
                SOLAR_OS_LOGW(TAG, "sleep: keyboard disconnect timeout");
                if (esp_hidh_dev_exists(dev)) {
                    (void)esp_hidh_dev_free_inner(dev);
                }
                clear_runtime_connection_state("sleep disconnect timeout");
                result = ESP_ERR_TIMEOUT;
            }
        }
    }

    clear_runtime_connection_state("sleep");
    reset_gatt_runtime_state("sleep");
    hid_gattc_if = ESP_GATT_IF_NONE;

    if (hidh_initialized) {
        const esp_err_t deinit_ret = esp_hidh_deinit();
        if (deinit_ret == ESP_OK || deinit_ret == ESP_ERR_INVALID_STATE) {
            hidh_initialized = false;
        } else {
            SOLAR_OS_LOGW(TAG, "sleep: HIDH deinit failed: %s", esp_err_to_name(deinit_ret));
            result = deinit_ret;
        }
    }

    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
        const esp_err_t disable_ret = esp_bluedroid_disable();
        if (disable_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "sleep: bluedroid disable failed: %s", esp_err_to_name(disable_ret));
            result = disable_ret;
        }
        bluedroid_status = esp_bluedroid_get_status();
    }
    if (bluedroid_status == ESP_BLUEDROID_STATUS_INITIALIZED) {
        const esp_err_t deinit_ret = esp_bluedroid_deinit();
        if (deinit_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "sleep: bluedroid deinit failed: %s", esp_err_to_name(deinit_ret));
            result = deinit_ret;
        }
    }

    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        const esp_err_t disable_ret = esp_bt_controller_disable();
        if (disable_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "sleep: controller disable failed: %s", esp_err_to_name(disable_ret));
            result = disable_ret;
        }
        controller_status = esp_bt_controller_get_status();
    }
    if (controller_status == ESP_BT_CONTROLLER_STATUS_INITED) {
        const esp_err_t deinit_ret = esp_bt_controller_deinit();
        if (deinit_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "sleep: controller deinit failed: %s", esp_err_to_name(deinit_ret));
            result = deinit_ret;
        }
    }

    initialized = false;
    set_status(BLE_KEYBOARD_IDLE, "sleep");
    return result;
}

void solar_os_ble_keyboard_resume(void)
{
    reconnect_suppressed_for_sleep = false;
    reconnect_suppressed_for_pairing = false;
    pairing_retry_pending = false;
    pairing_cancel_requested = false;

    const esp_err_t ret = solar_os_ble_keyboard_init();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "resume: BLE init failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "resume failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(BLE_KEYBOARD_RESUME_RECONNECT_DELAY_MS));

    if (!initialized || connected || remembered_peer_count() == 0) {
        return;
    }

    start_fast_reconnect_window("resume");
    memset(previous_keys, 0, sizeof(previous_keys));
    previous_modifiers = 0;
    repeat_clear();
    schedule_reconnect(0);
}

esp_err_t solar_os_ble_keyboard_forget(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ble_keyboard_peer_t forgotten_peers[BLE_KEYBOARD_MAX_REMEMBERED];
    memcpy(forgotten_peers, remembered_peers, sizeof(forgotten_peers));

    stop_reconnect_task("forget");

    esp_err_t ret = clear_remembered_peers();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "clear remembered keyboard failed: %s", esp_err_to_name(ret));
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_REMEMBERED; i++) {
        if (forgotten_peers[i].magic != BLE_KEYBOARD_PEER_MAGIC) {
            continue;
        }

        esp_err_t remove_ret = esp_ble_remove_bond_device(forgotten_peers[i].bda);
        if (remove_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "remove BLE bond %u failed: %s",
                          (unsigned)i,
                          esp_err_to_name(remove_ret));
        }
    }

    if (connected_dev != NULL) {
        esp_hidh_dev_close(connected_dev);
        set_status(BLE_KEYBOARD_IDLE, "forgetting keyboard");
    } else {
        set_status(BLE_KEYBOARD_IDLE, "forgot keyboard");
    }

    return ret;
}
