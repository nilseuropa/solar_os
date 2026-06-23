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
#include "solar_os_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#define BLE_KEYBOARD_SCAN_SECONDS 8
#define BLE_KEYBOARD_NAME_MAX 64
#define BLE_KEYBOARD_CHAR_QUEUE_LEN 128
#define BLE_KEYBOARD_MAX_KEYS 6
#define BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS 250
#define BLE_KEYBOARD_RECONNECT_FAST_RETRY_DELAY_MS 250
#define BLE_KEYBOARD_RECONNECT_FAST_WINDOW_MS 5000
#define BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS 1000
#define BLE_KEYBOARD_CONN_MIN_INTERVAL 6U
#define BLE_KEYBOARD_CONN_MAX_INTERVAL 12U
#define BLE_KEYBOARD_CONN_LATENCY 0U
#define BLE_KEYBOARD_CONN_TIMEOUT 400U
#define BLE_KEYBOARD_PEER_MAGIC 0x4b424431U
#define BLE_KEYBOARD_NVS_NAMESPACE "blekbd"
#define BLE_KEYBOARD_NVS_PEER_KEY "peer"
#define BLE_KEYBOARD_NVS_LAYOUT_KEY "layout"
#define BLE_KEYBOARD_NVS_REPEAT_RATE_KEY "repeat_cps"
#define BLE_KEYBOARD_NVS_REPEAT_DELAY_KEY "repeat_delay"
#define BLE_KEYBOARD_REPEAT_RATE_DEFAULT 15U
#define BLE_KEYBOARD_REPEAT_DELAY_DEFAULT_MS 450U
#define BLE_KEYBOARD_REPEAT_SEQUENCE_MAX 2U
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
    BLE_KEYBOARD_FAILED,
} ble_keyboard_state_t;

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

static const char *TAG = "ble_keyboard";

static SemaphoreHandle_t scan_done_sem;
static SemaphoreHandle_t close_done_sem;
static SemaphoreHandle_t status_mutex;
static QueueHandle_t char_queue;
static TaskHandle_t scan_task_handle;
static TaskHandle_t reconnect_task_handle;
static TickType_t reconnect_fast_until_tick;
static portMUX_TYPE repeat_lock = portMUX_INITIALIZER_UNLOCKED;
static bool initialized;
static bool connected;
static bool reconnect_suppressed_for_sleep;
static bool caps_lock;
static uint8_t previous_keys[BLE_KEYBOARD_MAX_KEYS];
static uint8_t previous_modifiers;
static esp_hidh_dev_t *connected_dev;
static ble_keyboard_state_t state = BLE_KEYBOARD_IDLE;
static ble_keyboard_candidate_t candidate;
static ble_keyboard_peer_t remembered_peer;
static solar_os_ble_keyboard_layout_t keyboard_layout = SOLAR_OS_BLE_KEYBOARD_LAYOUT_US;
static uint16_t repeat_rate_cps = BLE_KEYBOARD_REPEAT_RATE_DEFAULT;
static uint16_t repeat_delay_ms = BLE_KEYBOARD_REPEAT_DELAY_DEFAULT_MS;
static ble_keyboard_repeat_state_t repeat_state;
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

static void request_low_latency_conn_params(const uint8_t *bda, const char *phase)
{
    if (bda == NULL) {
        return;
    }

    esp_ble_conn_update_params_t params = {
        .min_int = BLE_KEYBOARD_CONN_MIN_INTERVAL,
        .max_int = BLE_KEYBOARD_CONN_MAX_INTERVAL,
        .latency = BLE_KEYBOARD_CONN_LATENCY,
        .timeout = BLE_KEYBOARD_CONN_TIMEOUT,
    };
    memcpy(params.bda, bda, sizeof(params.bda));

    const esp_err_t ret = esp_ble_gap_update_conn_params(&params);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG,
                 "%s: BLE conn param update failed: %s",
                 phase != NULL ? phase : "ble",
                 esp_err_to_name(ret));
    } else {
        SOLAR_OS_LOGI(TAG,
                 "%s: requested BLE conn interval 7.5-15 ms latency 0",
                 phase != NULL ? phase : "ble");
    }
}

static void set_preferred_low_latency_conn_params(const uint8_t *bda)
{
    if (bda == NULL) {
        return;
    }

    esp_bd_addr_t addr;
    memcpy(addr, bda, sizeof(addr));

    const esp_err_t ret = esp_ble_gap_set_prefer_conn_params(addr,
                                                             BLE_KEYBOARD_CONN_MIN_INTERVAL,
                                                             BLE_KEYBOARD_CONN_MAX_INTERVAL,
                                                             BLE_KEYBOARD_CONN_LATENCY,
                                                             BLE_KEYBOARD_CONN_TIMEOUT);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "preferred BLE conn params failed: %s", esp_err_to_name(ret));
    }
}

static void clear_runtime_connection_state(const char *reason)
{
    connected = false;
    connected_dev = NULL;
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

static bool remembered_peer_valid(void)
{
    return remembered_peer.magic == BLE_KEYBOARD_PEER_MAGIC;
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

static esp_err_t load_remembered_peer(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(&remembered_peer, 0, sizeof(remembered_peer));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t len = sizeof(remembered_peer);
    ret = nvs_get_blob(nvs, BLE_KEYBOARD_NVS_PEER_KEY, &remembered_peer, &len);
    nvs_close(nvs);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(&remembered_peer, 0, sizeof(remembered_peer));
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        memset(&remembered_peer, 0, sizeof(remembered_peer));
        return ret;
    }
    if (len != sizeof(remembered_peer) || !remembered_peer_valid()) {
        memset(&remembered_peer, 0, sizeof(remembered_peer));
        return ESP_ERR_INVALID_SIZE;
    }

    SOLAR_OS_LOGI(TAG,
             "remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
             ESP_BD_ADDR_HEX(remembered_peer.bda),
             addr_type_name((esp_ble_addr_type_t)remembered_peer.addr_type),
             remembered_peer.name[0] ? remembered_peer.name : "(unnamed)");
    return ESP_OK;
}

static esp_err_t save_remembered_peer(const uint8_t *bda, esp_ble_addr_type_t addr_type, const char *name)
{
    ble_keyboard_peer_t peer = {
        .magic = BLE_KEYBOARD_PEER_MAGIC,
        .addr_type = (uint8_t)addr_type,
    };
    memcpy(peer.bda, bda, sizeof(peer.bda));
    strlcpy(peer.name, name != NULL && name[0] ? name : "keyboard", sizeof(peer.name));
    remembered_peer = peer;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "open BLE keyboard NVS failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_blob(nvs, BLE_KEYBOARD_NVS_PEER_KEY, &peer, sizeof(peer));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG,
                 "remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
                 ESP_BD_ADDR_HEX(peer.bda),
                 addr_type_name((esp_ble_addr_type_t)peer.addr_type),
                 peer.name);
    } else {
        SOLAR_OS_LOGW(TAG, "save BLE keyboard peer failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t clear_remembered_peer(void)
{
    memset(&remembered_peer, 0, sizeof(remembered_peer));

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_erase_key(nvs, BLE_KEYBOARD_NVS_PEER_KEY);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
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

    set_preferred_low_latency_conn_params(bda);

    if (esp_hidh_dev_open(pending_bda, ESP_HID_TRANSPORT_BLE, pending_addr_type) == NULL) {
        SOLAR_OS_LOGE(TAG, "esp_hidh_dev_open failed");
        set_status(BLE_KEYBOARD_FAILED, "connect failed");
        return ESP_FAIL;
    }

    return ESP_OK;
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
    if (!has_hid_service && !keyboard_like) {
        return;
    }

    if (candidate.valid &&
        (!keyboard_like || candidate.keyboard_like) &&
        param->scan_rst.rssi <= candidate.rssi) {
        return;
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

static bool hid_should_prefix_alt(uint8_t modifiers, char ch)
{
    return (modifiers & HID_MOD_LEFT_ALT) != 0 &&
        ch != '\0' &&
        (uint8_t)ch != SOLAR_OS_KEY_APP_EXIT;
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

        const char ch = hid_keycode_to_char(key, modifiers);
        if (ch != '\0') {
            char sequence[BLE_KEYBOARD_REPEAT_SEQUENCE_MAX];
            size_t sequence_len = 0;

            if (hid_should_prefix_alt(modifiers, ch)) {
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
            request_low_latency_conn_params(param->ble_security.auth_cmpl.bd_addr, "auth");
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

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;

    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.status == ESP_OK) {
            connected = true;
            connected_dev = param->open.dev;
            memset(previous_keys, 0, sizeof(previous_keys));
            previous_modifiers = 0;
            repeat_clear();
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            const char *name = esp_hidh_dev_name_get(param->open.dev);
            const char *display_name = name != NULL && name[0] ? name : pending_name;
            strlcpy(connected_name, display_name[0] ? display_name : "keyboard", sizeof(connected_name));
            if (bda != NULL) {
                SOLAR_OS_LOGI(TAG, ESP_BD_ADDR_STR " open: %s",
                         ESP_BD_ADDR_HEX(bda),
                         connected_name);
                esp_gap_conn_params_t params = {0};
                if (esp_ble_get_current_conn_params((uint8_t *)bda, &params) == ESP_OK) {
                    log_conn_params("open", &params);
                }
                request_low_latency_conn_params(bda, "open");
                save_remembered_peer(bda, pending_addr_type, connected_name);
            } else {
                SOLAR_OS_LOGI(TAG, "open: %s", connected_name);
                request_low_latency_conn_params(pending_bda, "open");
                save_remembered_peer(pending_bda, pending_addr_type, connected_name);
            }
            esp_hidh_dev_dump(param->open.dev, stdout);
            set_status(BLE_KEYBOARD_CONNECTED, "connected %s", connected_name);
        } else {
            connected = false;
            connected_dev = NULL;
            memset(previous_keys, 0, sizeof(previous_keys));
            previous_modifiers = 0;
            SOLAR_OS_LOGE(TAG, "open failed: %s", esp_err_to_name(param->open.status));
            set_status(BLE_KEYBOARD_FAILED, "open failed");
            schedule_reconnect(BLE_KEYBOARD_RECONNECT_RETRY_DELAY_MS);
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
        connected = false;
        connected_dev = NULL;
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
        if (!reconnect_suppressed_for_sleep) {
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

    scan_done_sem = xSemaphoreCreateBinary();
    close_done_sem = xSemaphoreCreateBinary();
    status_mutex = xSemaphoreCreateMutex();
    char_queue = xQueueCreate(BLE_KEYBOARD_CHAR_QUEUE_LEN, sizeof(char));
    if (status_mutex == NULL ||
        scan_done_sem == NULL ||
        close_done_sem == NULL ||
        char_queue == NULL) {
        set_status(BLE_KEYBOARD_FAILED, "ble no memory");
        return ESP_ERR_NO_MEM;
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
    esp_err_t peer_ret = load_remembered_peer();
    if (peer_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load remembered keyboard failed: %s", esp_err_to_name(peer_ret));
    }

    esp_err_t ret = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        SOLAR_OS_LOGW(TAG, "classic bt memory release failed: %s", esp_err_to_name(ret));
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller enable failed");

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bluedroid_cfg.ssp_en = false;
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_cfg), TAG, "bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable failed");

    ESP_RETURN_ON_ERROR(esp_ble_gap_register_callback(gap_callback), TAG, "gap callback failed");
    ESP_RETURN_ON_ERROR(esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler),
                        TAG, "gattc callback failed");
    ESP_RETURN_ON_ERROR(init_security(), TAG, "security init failed");

    esp_hidh_config_t hidh_config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_RETURN_ON_ERROR(esp_hidh_init(&hidh_config), TAG, "hid host init failed");

    initialized = true;
    set_status(BLE_KEYBOARD_IDLE, "idle");
    if (remembered_peer_valid()) {
        start_fast_reconnect_window("boot");
        schedule_reconnect(0);
    }
    SOLAR_OS_LOGI(TAG, "BLE keyboard host ready");
    return ESP_OK;
}

static void scan_task(void *arg)
{
    (void)arg;

    while (xSemaphoreTake(scan_done_sem, 0) == pdTRUE) {
    }

    memset(&candidate, 0, sizeof(candidate));
    set_status(BLE_KEYBOARD_SCANNING, "scanning");
    SOLAR_OS_LOGI(TAG, "scan start");

    esp_err_t ret = esp_ble_gap_set_scan_params(&scan_params);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "set scan params failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "scan setup failed");
        scan_task_handle = NULL;
        vTaskDelete(NULL);
    }

    if (xSemaphoreTake(scan_done_sem, pdMS_TO_TICKS(1000)) != pdTRUE) {
        SOLAR_OS_LOGE(TAG, "scan params timeout");
        set_status(BLE_KEYBOARD_FAILED, "scan setup timeout");
        scan_task_handle = NULL;
        vTaskDelete(NULL);
    }

    ret = esp_ble_gap_start_scanning(BLE_KEYBOARD_SCAN_SECONDS);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "scan start failed");
        scan_task_handle = NULL;
        vTaskDelete(NULL);
    }

    if (xSemaphoreTake(scan_done_sem, pdMS_TO_TICKS((BLE_KEYBOARD_SCAN_SECONDS + 2) * 1000)) != pdTRUE) {
        SOLAR_OS_LOGE(TAG, "scan timeout");
        esp_ble_gap_stop_scanning();
        set_status(BLE_KEYBOARD_FAILED, "scan timeout");
        scan_task_handle = NULL;
        vTaskDelete(NULL);
    }

    if (!candidate.valid) {
        SOLAR_OS_LOGW(TAG, "no BLE HID keyboard candidate found");
        set_status(BLE_KEYBOARD_IDLE, "no keyboard found");
        scan_task_handle = NULL;
        vTaskDelete(NULL);
    }

    SOLAR_OS_LOGI(TAG,
             "connecting " ESP_BD_ADDR_STR " addr_type=%s name=%s",
             ESP_BD_ADDR_HEX(candidate.bda),
             addr_type_name(candidate.addr_type),
             candidate.name[0] ? candidate.name : "(none)");
    set_status(BLE_KEYBOARD_CONNECTING,
               "connecting %s",
               candidate.name[0] ? candidate.name : "keyboard");

    open_keyboard(candidate.bda, candidate.addr_type, candidate.name, "connecting");

    scan_task_handle = NULL;
    vTaskDelete(NULL);
}

static void reconnect_task(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;

    if (delay_ms > 0) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    while (initialized && remembered_peer_valid() && !connected) {
        if (scan_task_handle == NULL &&
            state != BLE_KEYBOARD_SCANNING &&
            state != BLE_KEYBOARD_CONNECTING &&
            state != BLE_KEYBOARD_PASSKEY) {
            SOLAR_OS_LOGI(TAG,
                     "reconnecting remembered keyboard " ESP_BD_ADDR_STR " addr_type=%s name=%s",
                     ESP_BD_ADDR_HEX(remembered_peer.bda),
                     addr_type_name((esp_ble_addr_type_t)remembered_peer.addr_type),
                     remembered_peer.name[0] ? remembered_peer.name : "(unnamed)");
            if (open_keyboard(remembered_peer.bda,
                              (esp_ble_addr_type_t)remembered_peer.addr_type,
                              remembered_peer.name,
                              "reconnecting") == ESP_OK) {
                break;
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
    if (!initialized || connected || !remembered_peer_valid() || reconnect_suppressed_for_sleep) {
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

    if (scan_task_handle != NULL || state == BLE_KEYBOARD_SCANNING || state == BLE_KEYBOARD_CONNECTING) {
        return ESP_ERR_INVALID_STATE;
    }

    if (connected) {
        set_status(BLE_KEYBOARD_CONNECTED, "already connected");
        return ESP_OK;
    }

    if (xTaskCreate(scan_task, "ble_kbd_scan", 6144, NULL, 4, &scan_task_handle) != pdPASS) {
        scan_task_handle = NULL;
        set_status(BLE_KEYBOARD_FAILED, "scan task failed");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms)
{
    if (!initialized) {
        return ESP_OK;
    }

    reconnect_suppressed_for_sleep = true;
    reconnect_fast_until_tick = 0;

    if (reconnect_task_handle != NULL) {
        TaskHandle_t task = reconnect_task_handle;
        reconnect_task_handle = NULL;
        vTaskDelete(task);
    }

    while (close_done_sem != NULL && xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
    }

    if (!connected || connected_dev == NULL) {
        return ESP_OK;
    }

    SOLAR_OS_LOGI(TAG, "sleep: closing keyboard connection");
    esp_hidh_dev_close(connected_dev);

    if (close_done_sem == NULL || timeout_ms == 0) {
        return ESP_OK;
    }

    if (xSemaphoreTake(close_done_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        SOLAR_OS_LOGI(TAG, "sleep: keyboard disconnected");
        return ESP_OK;
    }

    SOLAR_OS_LOGW(TAG, "sleep: keyboard disconnect timeout");
    clear_runtime_connection_state("sleep disconnect timeout");
    return ESP_ERR_TIMEOUT;
}

void solar_os_ble_keyboard_resume(void)
{
    reconnect_suppressed_for_sleep = false;

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_wakeup_request();
    }

    if (!initialized || connected || !remembered_peer_valid()) {
        return;
    }

    start_fast_reconnect_window("resume");
    schedule_reconnect(0);
}

esp_err_t solar_os_ble_keyboard_forget(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_bd_addr_t forgotten_bda = {0};
    const bool had_peer = remembered_peer_valid();
    if (had_peer) {
        memcpy(forgotten_bda, remembered_peer.bda, sizeof(forgotten_bda));
    }

    if (reconnect_task_handle != NULL) {
        TaskHandle_t task = reconnect_task_handle;
        reconnect_task_handle = NULL;
        vTaskDelete(task);
    }

    esp_err_t ret = clear_remembered_peer();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "clear remembered keyboard failed: %s", esp_err_to_name(ret));
    }

    if (had_peer) {
        esp_err_t remove_ret = esp_ble_remove_bond_device(forgotten_bda);
        if (remove_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "remove BLE bond failed: %s", esp_err_to_name(remove_ret));
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
