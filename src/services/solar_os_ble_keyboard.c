#include "solar_os_ble_keyboard.h"
#include "solar_os_ble_backend.h"
#include "solar_os_ble_nimble.h"
#include "solar_os_ble_hid.h"
#include "nimble/nimble_port.h"
#include "host/ble_store.h"
#include "store/config/ble_store_config.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "esp_random.h"

/* ESP-IDF exposes this initializer from ble_store_config.c, but not its header. */
void ble_store_config_init(void);

#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_hid_common.h"
#include "esp_log.h"
#include "soc/soc_caps.h"
#include "solar_os_hid_keyboard_report.h"
#include "solar_os_ble_keyboard_scan_policy.h"
#include "solar_os_board.h"
#include "solar_os_log.h"
#include "solar_os_input.h"
#include "solar_os_power.h"
#include "solar_os_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef uint8_t ble_address_t[6];
#define SOLAR_OS_BLE_ADDRESS_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define SOLAR_OS_BLE_ADDRESS_ARGS(a) (a)[0], (a)[1], (a)[2], (a)[3], (a)[4], (a)[5]
#define BLE_KEYBOARD_SCAN_SECONDS 8
#define BLE_KEYBOARD_NAME_MAX SOLAR_OS_BLE_KEYBOARD_NAME_MAX
#define BLE_KEYBOARD_MAX_KEYS SOLAR_OS_BLE_KEYBOARD_MAX_PRESSED_KEYS
#define BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS 250
#define BLE_KEYBOARD_RECONNECT_BACKOFF_INITIAL_MS 1000
#define BLE_KEYBOARD_RECONNECT_BACKOFF_MAX_MS 5000
#define BLE_KEYBOARD_PAIR_SWITCH_DISCONNECT_TIMEOUT_MS 1200
#define BLE_KEYBOARD_RESUME_RECONNECT_DELAY_MS 100
#define BLE_KEYBOARD_STALE_CLOSE_TIMEOUT_MS 1500
#define BLE_KEYBOARD_PEER_MAGIC 0x4b424431U
#define BLE_KEYBOARD_MAX_REMEMBERED SOLAR_OS_BLE_KEYBOARD_MAX_REMEMBERED
#define BLE_KEYBOARD_NVS_MIGRATE_MAX_REMEMBERED 3
#define BLE_KEYBOARD_NVS_NAMESPACE "blekbd"
#define BLE_KEYBOARD_NVS_PEERS_KEY "peers"
#define BLE_KEYBOARD_NVS_LEGACY_PEER_KEY "peer"
#define BLE_KEYBOARD_NVS_LAYOUT_KEY "layout"
#define BLE_KEYBOARD_NVS_ENABLED_KEY "enabled"

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
    BLE_KEYBOARD_SCAN_RECONNECT,
} ble_keyboard_scan_mode_t;

typedef struct {
    bool valid;
    bool keyboard_like;
    ble_address_t bda;
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    char name[BLE_KEYBOARD_NAME_MAX];
} ble_keyboard_candidate_t;

typedef struct {
    uint32_t magic;
    ble_address_t bda;
    uint8_t addr_type;
    char name[BLE_KEYBOARD_NAME_MAX];
} ble_keyboard_peer_t;

static const char *TAG = "ble_keyboard";

static SemaphoreHandle_t scan_done_sem;
static SemaphoreHandle_t scan_stop_done_sem;
static SemaphoreHandle_t close_done_sem;
static SemaphoreHandle_t status_mutex;
static TaskHandle_t scan_task_handle;
static TaskHandle_t reconnect_task_handle;
static portMUX_TYPE key_state_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE reconnect_task_lock = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE bond_remove_lock = portMUX_INITIALIZER_UNLOCKED;
static bool reconnect_stop_requested;
static bool reconnect_open_in_progress;
static ble_address_t deferred_forget_bda;
static bool deferred_forget_valid;
static bool initialized;
/* Freeze the current-boot policy before shell changes update the next boot. */
static bool boot_policy_loaded;
static bool enabled_for_current_boot = SOLAR_OS_BOARD_DEFAULT_BLE_ENABLED != 0;
static bool enabled_for_next_boot = SOLAR_OS_BOARD_DEFAULT_BLE_ENABLED != 0;
static solar_os_ble_keyboard_boot_setting_t next_boot_setting =
    SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT;
static bool disabled_boot_memory_release_attempted;
static esp_err_t disabled_boot_memory_release_result = ESP_OK;
static bool connected;
static bool reconnect_suppressed_for_sleep;
static bool reconnect_suppressed_for_pairing;
static bool reconnect_suppressed_for_forget;
static bool pairing_retry_pending;
static bool pairing_scan_stop_requested;
static bool reconnect_scan_stop_requested;
static bool reconnect_scan_stop_succeeded;
static bool candidate_frozen;
static ble_keyboard_scan_mode_t active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
static bool caps_lock;
static uint8_t previous_keys[BLE_KEYBOARD_MAX_KEYS];
static uint8_t previous_modifiers;
static solar_os_hid_keyboard_report_tracker_t keyboard_report_tracker;
static solar_os_input_source_t input_source;
static solar_os_ble_keyboard_key_state_t key_state;
static solar_os_ble_hid_device_t *connected_dev;
static solar_os_ble_hid_device_t *pending_dev;
static TickType_t pending_open_started_tick;
static ble_keyboard_state_t state = BLE_KEYBOARD_IDLE;
static ble_keyboard_candidate_t candidate;
static solar_os_ble_keyboard_scan_result_t *active_scan_results;
static size_t active_scan_max_results;
static size_t active_scan_result_count;
static ble_keyboard_peer_t remembered_peers[BLE_KEYBOARD_MAX_REMEMBERED];
static ble_address_t pending_bda;
static uint8_t pending_addr_type;
static char pending_name[BLE_KEYBOARD_NAME_MAX];
static char connected_name[BLE_KEYBOARD_NAME_MAX];
static char status_text[80] = "idle";
static bool bond_remove_pending;
static ble_address_t bond_remove_bda;

static esp_err_t init_nvs(void);
static void set_status(ble_keyboard_state_t next_state, const char *fmt, ...);

static esp_err_t load_boot_policy(void)
{
    if (boot_policy_loaded) {
        return ESP_OK;
    }

    enabled_for_current_boot = solar_os_ble_keyboard_board_default_enabled();
    enabled_for_next_boot = enabled_for_current_boot;
    next_boot_setting = SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT;

    esp_err_t ret = init_nvs();
    if (ret != ESP_OK) {
        boot_policy_loaded = true;
        return ret;
    }

    nvs_handle_t nvs;
    ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        boot_policy_loaded = true;
        return ret;
    }

    uint8_t stored = 1U;
    ret = nvs_get_u8(nvs, BLE_KEYBOARD_NVS_ENABLED_KEY, &stored);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        boot_policy_loaded = true;
        return ret;
    }

    enabled_for_current_boot = stored != 0U;
    enabled_for_next_boot = enabled_for_current_boot;
    next_boot_setting = enabled_for_current_boot
                            ? SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED
                            : SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED;
    boot_policy_loaded = true;
    return ESP_OK;
}

bool solar_os_ble_keyboard_enabled_for_current_boot(void)
{
    const esp_err_t ret = load_boot_policy();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load BLE boot policy failed; using board default: %s",
                      esp_err_to_name(ret));
    }
    return enabled_for_current_boot;
}

bool solar_os_ble_keyboard_enabled_for_next_boot(void)
{
    (void)solar_os_ble_keyboard_enabled_for_current_boot();
    return enabled_for_next_boot;
}

bool solar_os_ble_keyboard_board_default_enabled(void)
{
    return SOLAR_OS_BOARD_DEFAULT_BLE_ENABLED != 0;
}

solar_os_ble_keyboard_boot_setting_t solar_os_ble_keyboard_boot_setting(void)
{
    (void)solar_os_ble_keyboard_enabled_for_current_boot();
    return next_boot_setting;
}

const char *solar_os_ble_keyboard_boot_setting_name(
    solar_os_ble_keyboard_boot_setting_t setting)
{
    switch (setting) {
    case SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT:
        return "default";
    case SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED:
        return "on";
    case SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED:
        return "off";
    default:
        return "unknown";
    }
}

bool solar_os_ble_keyboard_parse_boot_setting(
    const char *name,
    solar_os_ble_keyboard_boot_setting_t *setting)
{
    if (name == NULL || setting == NULL) {
        return false;
    }
    if (strcmp(name, "default") == 0) {
        *setting = SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT;
        return true;
    }
    if (strcmp(name, "on") == 0 || strcmp(name, "enable") == 0 ||
        strcmp(name, "enabled") == 0) {
        *setting = SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED;
        return true;
    }
    if (strcmp(name, "off") == 0 || strcmp(name, "disable") == 0 ||
        strcmp(name, "disabled") == 0) {
        *setting = SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED;
        return true;
    }
    return false;
}

esp_err_t solar_os_ble_keyboard_set_boot_setting(
    solar_os_ble_keyboard_boot_setting_t setting)
{
    if (setting != SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT &&
        setting != SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED &&
        setting != SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    (void)solar_os_ble_keyboard_enabled_for_current_boot();

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    if (setting == SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT) {
        ret = nvs_erase_key(nvs, BLE_KEYBOARD_NVS_ENABLED_KEY);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ret = ESP_OK;
        }
    } else {
        ret = nvs_set_u8(nvs,
                         BLE_KEYBOARD_NVS_ENABLED_KEY,
                         setting == SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED ? 1U : 0U);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        next_boot_setting = setting;
        enabled_for_next_boot = setting == SOLAR_OS_BLE_KEYBOARD_BOOT_DEFAULT
                                    ? solar_os_ble_keyboard_board_default_enabled()
                                    : setting == SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED;
    }
    return ret;
}

esp_err_t solar_os_ble_keyboard_set_enabled_for_next_boot(bool enabled)
{
    return solar_os_ble_keyboard_set_boot_setting(
        enabled ? SOLAR_OS_BLE_KEYBOARD_BOOT_ENABLED
                : SOLAR_OS_BLE_KEYBOARD_BOOT_DISABLED);
}

esp_err_t solar_os_ble_keyboard_apply_boot_policy(void)
{
    if (solar_os_ble_keyboard_enabled_for_current_boot()) {
        return ESP_OK;
    }

    set_status(BLE_KEYBOARD_IDLE, "disabled for this boot");
    if (disabled_boot_memory_release_attempted) {
        return disabled_boot_memory_release_result;
    }
    disabled_boot_memory_release_attempted = true;

#if defined(SOC_BT_CLASSIC_SUPPORTED) && SOC_BT_CLASSIC_SUPPORTED
    const esp_bt_mode_t release_mode = ESP_BT_MODE_BTDM;
    const char *release_mode_name = "BTDM";
#else
    const esp_bt_mode_t release_mode = ESP_BT_MODE_BLE;
    const char *release_mode_name = "BLE";
#endif

    const size_t before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    disabled_boot_memory_release_result = esp_bt_mem_release(release_mode);
    if (disabled_boot_memory_release_result == ESP_OK) {
        const size_t after =
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        const size_t released = after >= before ? after - before : 0U;
        ESP_LOGI(TAG,
                 "BLE disabled for this boot; released %u bytes of %s memory",
                 (unsigned)released,
                 release_mode_name);
    }

    return disabled_boot_memory_release_result;
}

static void keyboard_report_state_reset(bool is_connected)
{
    if (input_source != SOLAR_OS_INPUT_SOURCE_INVALID) {
        solar_os_input_source_release_all(input_source);
        (void)solar_os_input_keyboard_source_set_ready(input_source, is_connected);
    }
    memset(previous_keys, 0, sizeof(previous_keys));
    previous_modifiers = 0;
    solar_os_hid_keyboard_report_reset(&keyboard_report_tracker);

    portENTER_CRITICAL(&key_state_lock);
    memset(&key_state, 0, sizeof(key_state));
    key_state.connected = is_connected;
    portEXIT_CRITICAL(&key_state_lock);
}

static struct ble_gap_disc_params scan_params = {
    .itvl = 80, .window = 48, .passive = 0, .filter_duplicates = 0,
};
static SemaphoreHandle_t host_synced;
static SemaphoreHandle_t host_stopped;
static TaskHandle_t host_task_handle;
static esp_err_t stop_scanning(void)
{
    int rc = ble_gap_disc_cancel();
    bool ok = rc == 0 || rc == BLE_HS_EALREADY;
    if (reconnect_scan_stop_requested) {
        reconnect_scan_stop_succeeded = ok;
        xSemaphoreGive(scan_stop_done_sem);
    }
    return ok ? ESP_OK : solar_os_ble_nimble_error(rc);
}

static const char *keyboard_layout_names[] = {
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_US] = "us",
    [SOLAR_OS_BLE_KEYBOARD_LAYOUT_DE] = "de",
};

static const char *addr_type_name(uint8_t addr_type);
static void set_status(ble_keyboard_state_t next_state, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void schedule_reconnect(uint32_t delay_ms);
static bool hidh_conn_params_ready(solar_os_ble_hid_device_t *dev, struct ble_gap_conn_desc *params);
static bool drop_existing_hidh_device(const uint8_t *bda, const char *reason, uint32_t timeout_ms);
static void restore_status_after_scan(ble_keyboard_scan_mode_t mode);
static esp_err_t run_keyboard_scan(ble_keyboard_scan_mode_t mode,
                                   ble_keyboard_candidate_t *selected_candidate);
static esp_err_t start_pairing_scan_now(void);
static void request_pairing_after_pending_connect(const char *reason);
static bool deferred_bond_forget_pending(void);
static bool forget_operation_pending(void);
static esp_err_t remove_deferred_bonds(void);
static esp_err_t complete_bond_forget(const uint8_t *bda, esp_err_t result);
static void log_conn_params(const char *prefix, const struct ble_gap_conn_desc *params)
{
    SOLAR_OS_LOGI(TAG, "%s conn interval=%u us latency=%u timeout=%u ms",
        prefix, (unsigned)params->conn_itvl * 1250U,
        (unsigned)params->conn_latency, (unsigned)params->supervision_timeout * 10U);
}

static void clear_runtime_connection_state(const char *reason)
{
    connected = false;
    connected_dev = NULL;
    pending_dev = NULL;
    pending_open_started_tick = 0;
    keyboard_report_state_reset(false);
    if (reason != NULL) {
        set_status(BLE_KEYBOARD_IDLE, "%s", reason);
    }
}

static bool reconnect_is_suppressed(void)
{
    return reconnect_suppressed_for_sleep ||
        reconnect_suppressed_for_pairing ||
        reconnect_suppressed_for_forget ||
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

static bool stop_reconnect_task(const char *reason, uint32_t timeout_ms)
{
    TaskHandle_t task = NULL;

    /* Cancel setup and let the waiter release its ownership normally. Never
     * delete a worker while its open call still pins the HID connection. */
    portENTER_CRITICAL(&reconnect_task_lock);
    task = reconnect_task_handle;
    if (task != NULL) {
        reconnect_stop_requested = true;
    }
    portEXIT_CRITICAL(&reconnect_task_lock);

    solar_os_ble_hid_cancel_open();
    if (task == NULL) {
        return true;
    }

    if (state == BLE_KEYBOARD_SCANNING) {
        SOLAR_OS_LOGI(TAG,
                      "%s: stopping reconnect scan",
                      reason != NULL ? reason : "ble");
        (void)stop_scanning();
        if (scan_done_sem != NULL) {
            xSemaphoreGive(scan_done_sem);
        }
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        set_status(BLE_KEYBOARD_IDLE, "%s", reason != NULL ? reason : "idle");
    }

    SOLAR_OS_LOGI(TAG,
                  "%s: requesting reconnect task stop",
                  reason != NULL ? reason : "ble");
    xTaskNotifyGive(task);

    if (timeout_ms == 0U) {
        portENTER_CRITICAL(&reconnect_task_lock);
        const bool stopped = reconnect_task_handle == NULL;
        portEXIT_CRITICAL(&reconnect_task_lock);
        return stopped;
    }

    const TickType_t deadline =
        xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        portENTER_CRITICAL(&reconnect_task_lock);
        const bool stopped = reconnect_task_handle == NULL;
        portEXIT_CRITICAL(&reconnect_task_lock);
        if (stopped) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    portENTER_CRITICAL(&reconnect_task_lock);
    const bool open_in_progress = reconnect_open_in_progress;
    portEXIT_CRITICAL(&reconnect_task_lock);
    SOLAR_OS_LOGW(TAG,
                  "%s: reconnect task stop deferred%s",
                  reason != NULL ? reason : "ble",
                  open_in_progress ? " by active HID open" : "");
    return false;
}

static void stop_scan_task_for_sleep(uint32_t timeout_ms)
{
    if (scan_task_handle == NULL && state != BLE_KEYBOARD_SCANNING) {
        return;
    }

    pairing_scan_stop_requested = true;
    if (state == BLE_KEYBOARD_SCANNING) {
        SOLAR_OS_LOGI(TAG, "sleep: stopping BLE scan");
        (void)stop_scanning();
    }
    if (scan_done_sem != NULL) {
        xSemaphoreGive(scan_done_sem);
    }

    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (scan_task_handle != NULL && (int32_t)(deadline - xTaskGetTickCount()) > 0) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (scan_task_handle != NULL) {
        solar_os_ble_hid_cancel_open();
        SOLAR_OS_LOGI(TAG, "sleep: waiting for pairing worker");
        return;
    }

    pairing_scan_stop_requested = false;
    active_scan_results = NULL;
    active_scan_max_results = 0;
    active_scan_result_count = 0;
    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
    if (!connected) {
        set_status(BLE_KEYBOARD_IDLE, "sleep");
    }
}

static esp_err_t ensure_runtime_objects(void)
{
    if (scan_done_sem == NULL) {
        scan_done_sem = xSemaphoreCreateBinary();
    }
    if (scan_stop_done_sem == NULL) {
        scan_stop_done_sem = xSemaphoreCreateBinary();
    }
    if (close_done_sem == NULL) {
        close_done_sem = xSemaphoreCreateBinary();
    }
    if (status_mutex == NULL) {
        status_mutex = xSemaphoreCreateMutex();
    }
    const esp_err_t gatt_ret = solar_os_ble_service_prepare_runtime();
    if (status_mutex == NULL ||
        scan_done_sem == NULL ||
        scan_stop_done_sem == NULL ||
        close_done_sem == NULL || gatt_ret != ESP_OK) {
        set_status(BLE_KEYBOARD_FAILED, "ble no memory");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
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
            solar_os_ble_keyboard_scan_reconnect_bda_matches(
                remembered_peers[i].bda,
                bda)) {
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

    return solar_os_input_set_keyboard_layout(
        (solar_os_input_keyboard_layout_t)value);
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
                 "remembered keyboard " SOLAR_OS_BLE_ADDRESS_FMT " addr_type=%s name=%s",
                 SOLAR_OS_BLE_ADDRESS_ARGS(remembered_peers[i].bda),
                 addr_type_name((uint8_t)remembered_peers[i].addr_type),
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

static esp_err_t save_remembered_peer(const uint8_t *bda, uint8_t addr_type, const char *name)
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
                 "remembered keyboard " SOLAR_OS_BLE_ADDRESS_FMT " addr_type=%s name=%s",
                 SOLAR_OS_BLE_ADDRESS_ARGS(peer.bda),
                 addr_type_name((uint8_t)peer.addr_type),
                 peer.name);
    }

    return ret;
}

static esp_err_t clear_remembered_peers(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(BLE_KEYBOARD_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
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
    if (ret == ESP_OK) {
        memset(remembered_peers, 0, sizeof(remembered_peers));
    }
    return ret;
}

static bool hidh_conn_params_ready(solar_os_ble_hid_device_t *dev, struct ble_gap_conn_desc *params)
{
    struct ble_gap_conn_desc local;
    return dev && dev->conn_id >= 0 && ble_gap_conn_find(dev->conn_id, params ? params : &local) == 0;
}

static bool drop_existing_hidh_device(const uint8_t *bda, const char *reason, uint32_t timeout_ms)
{
    if (solar_os_ble_hid_idle()) return true;
    if (connected_dev) (void)solar_os_ble_hid_close(connected_dev);
    else solar_os_ble_hid_cancel_open();
    TickType_t start = xTaskGetTickCount();
    while (!solar_os_ble_hid_idle()) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

static esp_err_t open_keyboard(const uint8_t *bda,
                               uint8_t addr_type,
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
    solar_os_ble_hid_device_t *opened_dev = solar_os_ble_hid_open(pending_bda, pending_addr_type);
    if (opened_dev == NULL) {
        pending_open_started_tick = 0;
        SOLAR_OS_LOGE(TAG, "solar_os_ble_hid_open failed");
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
    if (connected) return true;
    solar_os_ble_hid_cancel_open();
    TickType_t start = xTaskGetTickCount();
    while (!solar_os_ble_hid_idle()) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    pending_dev = NULL;
    pending_open_started_tick = 0;
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
    if (input_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        return 0;
    }
    return solar_os_input_read_source_chars(input_source, buffer, buffer_len);
}

void solar_os_ble_keyboard_get_key_state(solar_os_ble_keyboard_key_state_t *out)
{
    if (out == NULL) {
        return;
    }

    portENTER_CRITICAL(&key_state_lock);
    *out = key_state;
    portEXIT_CRITICAL(&key_state_lock);
}

void solar_os_ble_keyboard_get_repeat(uint16_t *rate_cps, uint16_t *delay_ms)
{
    solar_os_input_get_repeat(rate_cps, delay_ms);
}

esp_err_t solar_os_ble_keyboard_set_repeat(uint16_t rate_cps, uint16_t delay_ms)
{
    return solar_os_input_set_repeat(rate_cps, delay_ms);
}

solar_os_ble_keyboard_layout_t solar_os_ble_keyboard_layout(void)
{
    return (solar_os_ble_keyboard_layout_t)solar_os_input_keyboard_layout();
}

esp_err_t solar_os_ble_keyboard_set_layout(solar_os_ble_keyboard_layout_t layout)
{
    if ((size_t)layout >= sizeof(keyboard_layout_names) / sizeof(keyboard_layout_names[0])) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = solar_os_input_set_keyboard_layout(
        (solar_os_input_keyboard_layout_t)layout);
    if (ret != ESP_OK) {
        return ret;
    }
    /* Keep the legacy key synchronized for downgrade compatibility. */
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
    return addr_type_name((uint8_t)addr_type);
}

bool solar_os_ble_keyboard_parse_addr_type(const char *name, uint8_t *addr_type)
{
    if (name == NULL || addr_type == NULL) {
        return false;
    }

    const uint8_t types[] = {
        BLE_ADDR_PUBLIC,
        BLE_ADDR_RANDOM,
        SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY,
        SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY,
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

static const char *addr_type_name(uint8_t addr_type)
{
    switch (addr_type) {
    case BLE_ADDR_PUBLIC:
        return "public";
    case BLE_ADDR_RANDOM:
        return "random";
    case SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY:
        return "rpa_public";
    case SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY:
        return "rpa_random";
    default:
        return "unknown";
    }
}

static const uint8_t *adv_field(const uint8_t *data, size_t len, uint8_t type, size_t *found_len)
{
    for (size_t i = 0; i < len;) {
        size_t n = data[i++];
        if (!n || n > len - i) break;
        if (data[i] == type) {
            *found_len = n - 1;
            return data + i + 1;
        }
        i += n;
    }
    *found_len = 0;
    return NULL;
}

static bool adv_has_uuid16(const uint8_t *data, size_t len, uint8_t type, uint16_t uuid)
{
    size_t n;
    const uint8_t *v = adv_field(data, len, type, &n);
    for (size_t i = 0; v && i + 1 < n; i += 2) {
        if (((uint16_t)v[i] | ((uint16_t)v[i + 1] << 8)) == uuid) return true;
    }
    return false;
}

static uint16_t adv_appearance(const uint8_t *data, size_t len)
{
    size_t n;
    const uint8_t *v = adv_field(data, len, 0x19, &n);
    return v && n == 2 ? ((uint16_t)v[0] | ((uint16_t)v[1] << 8)) : 0;
}

static void adv_name(const uint8_t *data, size_t len, char *name, size_t size)
{
    size_t n;
    const uint8_t *v = adv_field(data, len, 0x09, &n);
    if (!v) v = adv_field(data, len, 0x08, &n);
    if (!size) return;
    if (n >= size) n = size - 1;
    if (v && n) memcpy(name, v, n);
    name[n] = 0;
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
                                uint8_t addr_type,
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

    const uint8_t *bda = solar_os_ble_hid_address(connected_dev);
    if (bda == NULL) {
        return;
    }

    solar_os_ble_keyboard_scan_result_t *slot = scan_result_slot(bda);
    if (slot == NULL) {
        return;
    }

    const char *name = connected_name;
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

static void consider_candidate(const struct ble_gap_disc_desc *param)
{
    char name[BLE_KEYBOARD_NAME_MAX];
    const uint16_t adv_len = param->length_data;
    const uint8_t *adv_data = param->data;
    uint8_t bda[6];
    solar_os_ble_nimble_display_address(bda, &param->addr);
    const uint16_t appearance = adv_appearance(adv_data, adv_len);
    const bool has_hid_service =
        adv_has_uuid16(adv_data, adv_len, 0x03, 0x1812) ||
        adv_has_uuid16(adv_data, adv_len, 0x02, 0x1812);

    adv_name(adv_data, adv_len, name, sizeof(name));

    const bool keyboard_like = appearance == ESP_HID_APPEARANCE_KEYBOARD ||
        solar_os_ble_keyboard_scan_name_is_keyboard_like(name);
    if (active_scan_mode == BLE_KEYBOARD_SCAN_RECONNECT) {
        if (candidate_frozen) {
            return;
        }
        if (!bda_matches_remembered_peer(bda)) {
            return;
        }
        if (!solar_os_ble_keyboard_scan_reconnect_event_is_connectable(
                (uint8_t)param->event_type)) {
            return;
        }

        /* A remembered peer is already known to be the keyboard. */
        candidate.valid = true;
        candidate.keyboard_like = true;
        memcpy(candidate.bda, bda, sizeof(candidate.bda));
        candidate.addr_type = param->addr.type;
        candidate.rssi = param->rssi;
        candidate.appearance = appearance;
        strlcpy(candidate.name, name, sizeof(candidate.name));
        /* Scan results queued before stop completion must not replace this peer. */
        candidate_frozen = true;

        if (!reconnect_scan_stop_requested) {
            reconnect_scan_stop_requested = true;
            const esp_err_t stop_ret = stop_scanning();
            if (stop_ret != ESP_OK) {
                SOLAR_OS_LOGW(TAG,
                              "reconnect scan stop request failed: %s",
                              esp_err_to_name(stop_ret));
                reconnect_scan_stop_requested = false;
            }
        }
        return;
    }

    if (active_scan_mode == BLE_KEYBOARD_SCAN_DISCOVERY) {
        collect_scan_result(bda,
                            param->addr.type,
                            param->rssi,
                            appearance,
                            has_hid_service,
                            keyboard_like,
                            name);
    }

    if (!solar_os_ble_keyboard_scan_candidate_should_replace(
            candidate_frozen,
            candidate.valid,
            candidate.keyboard_like,
            candidate.rssi,
            has_hid_service,
            keyboard_like,
            param->rssi)) {
        return;
    }

    candidate.valid = true;
    candidate.keyboard_like = keyboard_like;
    memcpy(candidate.bda, bda, sizeof(candidate.bda));
    candidate.addr_type = param->addr.type;
    candidate.rssi = param->rssi;
    candidate.appearance = appearance;
    strlcpy(candidate.name, name, sizeof(candidate.name));

    SOLAR_OS_LOGI(TAG,
             "candidate " SOLAR_OS_BLE_ADDRESS_FMT " rssi=%d appearance=0x%04x addr_type=%s name=%s%s",
             SOLAR_OS_BLE_ADDRESS_ARGS(candidate.bda),
             candidate.rssi,
             candidate.appearance,
             addr_type_name(candidate.addr_type),
             candidate.name[0] ? candidate.name : "(none)",
             candidate.keyboard_like ? " keyboard-like" : "");
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

static void keyboard_report_state_publish(uint8_t modifiers, const uint8_t *keys)
{
    solar_os_ble_keyboard_key_state_t next = {
        .connected = connected,
        .modifiers = modifiers,
    };

    if (keys != NULL) {
        for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
            next.keycodes[i] = keys[i];
            next.chars[i] = solar_os_input_translate_hid_usage(keys[i],
                                                               modifiers,
                                                               caps_lock);
        }
    }

    portENTER_CRITICAL(&key_state_lock);
    key_state = next;
    portEXIT_CRITICAL(&key_state_lock);
}

static void handle_keyboard_report(uint8_t map_index,
                                   uint16_t report_id,
                                   const uint8_t *data,
                                   uint16_t length)
{
    solar_os_hid_keyboard_report_state_t report_state;
    if (!solar_os_hid_keyboard_report_update(&keyboard_report_tracker,
                                             map_index,
                                             report_id,
                                             data,
                                             length,
                                             &report_state)) {
        return;
    }

    const uint8_t modifiers = report_state.modifiers;
    const uint8_t *keys = report_state.keys;

    if (key_in_report(0x01, keys)) {
        keyboard_report_state_reset(connected);
        previous_modifiers = modifiers;
        return;
    }

    const uint8_t changed_modifiers = (uint8_t)(previous_modifiers ^ modifiers);
    for (uint8_t bit = 0U; bit < 8U; bit++) {
        const uint8_t mask = (uint8_t)(1U << bit);
        if ((changed_modifiers & mask) == 0U) {
            continue;
        }
        const uint16_t usage = (uint16_t)(0xe0U + bit);
        (void)solar_os_input_write_key(
            input_source,
            usage,
            usage,
            0U,
            modifiers,
            (modifiers & mask) != 0U ? SOLAR_OS_INPUT_KEY_PRESS :
                                      SOLAR_OS_INPUT_KEY_RELEASE);
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        const uint8_t key = previous_keys[i];
        if (key == 0 || key_in_report(key, keys)) {
            continue;
        }
        (void)solar_os_input_write_key(input_source,
                                       key,
                                       key,
                                       solar_os_input_translate_hid_usage(key,
                                                                          previous_modifiers,
                                                                          caps_lock),
                                       modifiers,
                                       SOLAR_OS_INPUT_KEY_RELEASE);
    }

    for (size_t i = 0; i < BLE_KEYBOARD_MAX_KEYS; i++) {
        const uint8_t key = keys[i];
        if (key == 0 || key_in_report(key, previous_keys)) {
            continue;
        }

        if (key == 0x39) {
            caps_lock = !caps_lock;
        }

        const uint8_t ch = solar_os_input_translate_hid_usage(key,
                                                              modifiers,
                                                              caps_lock);
        (void)solar_os_input_write_key(input_source,
                                       key,
                                       key,
                                       (uint8_t)ch,
                                       modifiers,
                                       SOLAR_OS_INPUT_KEY_PRESS);
    }

    keyboard_report_state_publish(modifiers, keys);
    memcpy(previous_keys, keys, sizeof(previous_keys));
    previous_modifiers = modifiers;
}

static int scan_callback(struct ble_gap_event *event, void *arg)
{
    if (event->type == BLE_GAP_EVENT_DISC) consider_candidate(&event->disc);
    else if (event->type == BLE_GAP_EVENT_DISC_COMPLETE) xSemaphoreGive(scan_done_sem);
    return 0;
}

int solar_os_ble_nimble_security(struct ble_gap_event *event)
{
    if (event->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
        struct ble_sm_io io = {.action = event->passkey.params.action};
        if (io.action == BLE_SM_IOACT_DISP) {
            io.passkey = esp_random() % 1000000U;
            SOLAR_OS_LOGI(TAG, "type passkey %06lu on the peer device, then Enter",
                          (unsigned long)io.passkey);
            set_status(BLE_KEYBOARD_PASSKEY, "peer passkey %06lu", (unsigned long)io.passkey);
            return ble_sm_inject_io(event->passkey.conn_handle, &io);
        }
        (void)ble_gap_terminate(event->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
        return 0;
    }
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
        SOLAR_OS_LOGW(TAG, "peer requests new keys; forget and pair again");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    return 0;
}

static void hidh_callback(solar_os_ble_hid_event_type_t id, solar_os_ble_hid_event_t *param)
{
    switch (id) {
    case SOLAR_OS_BLE_HID_OPEN:
        if (pending_dev == param->open.dev) {
            pending_dev = NULL;
        }
        pending_open_started_tick = 0;
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = solar_os_ble_hid_address(param->open.dev);
            struct ble_gap_conn_desc params = {0};
            if (bda == NULL) {
                SOLAR_OS_LOGW(TAG, "HID keyboard address unavailable at open; using requested address");
                bda = pending_bda;
            }

            const bool conn_params_ready =
                hidh_conn_params_ready(param->open.dev, &params);
            connected = true;
            connected_dev = param->open.dev;
            keyboard_report_state_reset(true);
            const char *name = pending_name;
            const char *display_name = name != NULL && name[0] ? name : pending_name;
            strlcpy(connected_name, display_name[0] ? display_name : "keyboard", sizeof(connected_name));
            SOLAR_OS_LOGI(TAG, SOLAR_OS_BLE_ADDRESS_FMT " open: %s",
                     SOLAR_OS_BLE_ADDRESS_ARGS(bda),
                     connected_name);
            if (conn_params_ready) {
                log_conn_params("open", &params);
            }
            if (pairing_retry_pending || forget_operation_pending()) {
                SOLAR_OS_LOGI(
                    TAG,
                    "not remembering keyboard superseded by forget/pair request");
            } else {
                save_remembered_peer(bda, param->open.dev->addr_type, connected_name);
            }
            set_status(BLE_KEYBOARD_CONNECTED, "connected %s", connected_name);
            if (pairing_retry_pending && !forget_operation_pending()) {
                portENTER_CRITICAL(&reconnect_task_lock);
                const bool reconnect_open = reconnect_open_in_progress;
                portEXIT_CRITICAL(&reconnect_task_lock);
                if (reconnect_open) {
                    SOLAR_OS_LOGI(
                        TAG,
                        "pairing waits for the HID open worker to finish");
                } else {
                    esp_err_t pair_ret = start_pairing_scan_now();
                    if (pair_ret != ESP_OK) {
                        SOLAR_OS_LOGW(
                            TAG,
                            "deferred pairing start failed after open: %s",
                            esp_err_to_name(pair_ret));
                    }
                }
            }
        } else {
            connected = false;
            connected_dev = NULL;
            pending_dev = NULL;
            keyboard_report_state_reset(false);
            SOLAR_OS_LOGE(TAG, "open failed: %s", esp_err_to_name(param->open.status));
            if (pairing_retry_pending && !forget_operation_pending()) {
                portENTER_CRITICAL(&reconnect_task_lock);
                const bool reconnect_open = reconnect_open_in_progress;
                portEXIT_CRITICAL(&reconnect_task_lock);
                if (!reconnect_open) {
                    esp_err_t pair_ret = start_pairing_scan_now();
                    if (pair_ret != ESP_OK) {
                        SOLAR_OS_LOGW(
                            TAG,
                            "deferred pairing start failed after open failure: %s",
                            esp_err_to_name(pair_ret));
                        set_status(BLE_KEYBOARD_FAILED, "open failed");
                    }
                }
                break;
            }
            set_status(BLE_KEYBOARD_FAILED, "open failed");
            if (!reconnect_is_suppressed()) {
                schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
            }
        }
        break;

    case SOLAR_OS_BLE_HID_BATTERY:
        SOLAR_OS_LOGI(TAG, "battery %u%%", param->battery.level);
        break;

    case SOLAR_OS_BLE_HID_INPUT:
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
            handle_keyboard_report(param->input.map_index,
                                   param->input.report_id,
                                   param->input.data,
                                   param->input.length);
            set_status(BLE_KEYBOARD_CONNECTED, "connected %s", connected_name[0] ? connected_name : "keyboard");
        }
        break;

    case SOLAR_OS_BLE_HID_CLOSE:
        pending_open_started_tick = 0;
        connected = false;
        if (connected_dev == param->close.dev) {
            connected_dev = NULL;
        }
        if (pending_dev == param->close.dev) {
            pending_dev = NULL;
        }
        keyboard_report_state_reset(false);
        SOLAR_OS_LOGI(TAG, "close reason=%d status=%s",
                 param->close.reason,
                 esp_err_to_name(param->close.status));
        const bool completing_forget = deferred_bond_forget_pending();
        if (completing_forget) {
            set_status(BLE_KEYBOARD_PAIRING_PENDING, "forget pending");
        }
        (void)remove_deferred_bonds();
        if (!completing_forget) {
            set_status(BLE_KEYBOARD_IDLE, "disconnected");
        }
        if (close_done_sem != NULL) {
            xSemaphoreGive(close_done_sem);
        }
        if (pairing_retry_pending && !forget_operation_pending()) {
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

static void host_sync(void)
{
    if (ble_hs_util_ensure_addr(0) == 0) xSemaphoreGive(host_synced);
}
static void host_reset(int reason)
{
    SOLAR_OS_LOGW(TAG, "NimBLE host reset: %d", reason);
}
static void host_task(void *arg)
{
    nimble_port_run();
    xSemaphoreGive(host_stopped);
    solar_os_task_delete_internal(NULL);
}

esp_err_t solar_os_ble_backend_init(void)
{
    if (initialized) {
        return ESP_OK;
    }

    if (!solar_os_ble_keyboard_enabled_for_current_boot()) {
        const esp_err_t release_ret = solar_os_ble_keyboard_apply_boot_policy();
        if (release_ret != ESP_OK) {
            return release_ret;
        }
        return ESP_ERR_NOT_ALLOWED;
    }

    ESP_RETURN_ON_ERROR(ensure_runtime_objects(), TAG, "runtime object setup failed");
    if (input_source == SOLAR_OS_INPUT_SOURCE_INVALID) {
        ESP_RETURN_ON_ERROR(solar_os_input_keyboard_source_open("ble-keyboard",
                                                               false,
                                                               &input_source),
                            TAG,
                            "input source setup failed");
    }

    ESP_RETURN_ON_ERROR(init_nvs(), TAG, "nvs init failed");
    esp_err_t layout_ret = load_keyboard_layout();
    if (layout_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load keyboard layout failed: %s", esp_err_to_name(layout_ret));
    }
    esp_err_t peer_ret = load_remembered_peers();
    if (peer_ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load remembered keyboard failed: %s", esp_err_to_name(peer_ret));
    }

    if (!host_synced) host_synced = xSemaphoreCreateBinary();
    if (!host_stopped) host_stopped = xSemaphoreCreateBinary();
    if (!host_synced || !host_stopped) return ESP_ERR_NO_MEM;
    while (xSemaphoreTake(host_synced, 0) == pdTRUE) {}
    while (xSemaphoreTake(host_stopped, 0) == pdTRUE) {}
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "NimBLE init failed");
    esp_err_t hid_ret = solar_os_ble_hid_init(hidh_callback);
    if (hid_ret != ESP_OK) {
        (void)nimble_port_deinit();
        return hid_ret;
    }
    ble_hs_cfg.sync_cb = host_sync;
    ble_hs_cfg.reset_cb = host_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    /* Stable standard services support discovery caching / Service Changed
     * for runtime application services. They do not start advertising. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_att_set_preferred_mtu(517);
    if (solar_os_task_create_pinned_internal(host_task, "nimble_host", 4096, NULL,
        configMAX_PRIORITIES - 4, &host_task_handle, 0, SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        (void)solar_os_ble_hid_deinit();
        (void)nimble_port_deinit();
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(host_synced, pdMS_TO_TICKS(5000)) != pdTRUE) {
        (void)nimble_port_stop();
        xSemaphoreTake(host_stopped, portMAX_DELAY);
        (void)solar_os_ble_hid_deinit();
        (void)nimble_port_deinit();
        return ESP_ERR_TIMEOUT;
    }
    (void)solar_os_power_apply_runtime_policy();

    ESP_RETURN_ON_ERROR(solar_os_ble_service_register(), TAG, "generic gatt register failed");

    initialized = true;
    set_status(BLE_KEYBOARD_IDLE, "idle");
    if (remembered_peer_count() > 0) {
        schedule_reconnect(0);
    }
    SOLAR_OS_LOGI(TAG, "BLE keyboard host ready");
    return ESP_OK;
}

esp_err_t solar_os_ble_backend_scan(solar_os_ble_keyboard_scan_result_t *results,
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

    const esp_err_t ret = run_keyboard_scan(BLE_KEYBOARD_SCAN_DISCOVERY, NULL);

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

static const char *scan_mode_status(ble_keyboard_scan_mode_t mode)
{
    switch (mode) {
    case BLE_KEYBOARD_SCAN_PAIRING:
        return "pairing";
    case BLE_KEYBOARD_SCAN_RECONNECT:
        return "waiting for keyboard";
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
    case BLE_KEYBOARD_SCAN_RECONNECT:
        return "remembered keyboard reconnect";
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
        } else if (mode == BLE_KEYBOARD_SCAN_RECONNECT) {
            message = "remembered keyboard unavailable; waiting";
        }
        set_status(BLE_KEYBOARD_IDLE, "%s", message);
    }
}

static esp_err_t run_keyboard_scan(ble_keyboard_scan_mode_t mode,
                                   ble_keyboard_candidate_t *selected_candidate)
{
    while (xSemaphoreTake(scan_done_sem, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(scan_stop_done_sem, 0) == pdTRUE) {
    }

    memset(&candidate, 0, sizeof(candidate));
    candidate_frozen = false;
    reconnect_scan_stop_requested = false;
    reconnect_scan_stop_succeeded = false;
    active_scan_mode = mode;
    set_status(BLE_KEYBOARD_SCANNING, "%s", scan_mode_status(mode));
    SOLAR_OS_LOGI(TAG, "%s scan start", scan_mode_log_name(mode));

    esp_err_t ret = solar_os_ble_nimble_error(ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
        BLE_KEYBOARD_SCAN_SECONDS * 1000, &scan_params, scan_callback, NULL));

    if (ret != ESP_OK) {
        SOLAR_OS_LOGE(TAG, "scan start failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "scan start failed");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ret;
    }

    const TickType_t scan_timeout =
        pdMS_TO_TICKS((BLE_KEYBOARD_SCAN_SECONDS + 2) * 1000);
    if (mode == BLE_KEYBOARD_SCAN_RECONNECT) {
        const TickType_t deadline = xTaskGetTickCount() + scan_timeout;
        for (;;) {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(deadline - now) <= 0) {
                SOLAR_OS_LOGE(TAG, "scan timeout");
                stop_scanning();
                set_status(BLE_KEYBOARD_FAILED, "scan timeout");
                active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
                reconnect_scan_stop_requested = false;
                return ESP_ERR_TIMEOUT;
            }

            if (reconnect_scan_stop_requested) {
                if (xSemaphoreTake(scan_stop_done_sem, deadline - now) != pdTRUE) {
                    SOLAR_OS_LOGE(TAG, "reconnect scan stop timeout");
                    set_status(BLE_KEYBOARD_FAILED, "scan stop timeout");
                    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
                    reconnect_scan_stop_requested = false;
                    return ESP_ERR_TIMEOUT;
                }
                if (!reconnect_scan_stop_succeeded) {
                    set_status(BLE_KEYBOARD_FAILED, "scan stop failed");
                    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
                    reconnect_scan_stop_requested = false;
                    return ESP_FAIL;
                }
                break;
            }

            TickType_t wait_ticks = deadline - now;
            if (wait_ticks > pdMS_TO_TICKS(100)) {
                wait_ticks = pdMS_TO_TICKS(100);
            }
            if (xSemaphoreTake(scan_done_sem, wait_ticks) == pdTRUE &&
                !reconnect_scan_stop_requested) {
                break;
            }
        }
    } else if (xSemaphoreTake(scan_done_sem, scan_timeout) != pdTRUE) {
        SOLAR_OS_LOGE(TAG, "scan timeout");
        stop_scanning();
        set_status(BLE_KEYBOARD_FAILED, "scan timeout");
        active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
        return ESP_ERR_TIMEOUT;
    }
    active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;
    reconnect_scan_stop_requested = false;

    if (!candidate.valid) {
        SOLAR_OS_LOGW(TAG,
                      "%s candidate not found",
                      scan_mode_log_name(mode));
        restore_status_after_scan(mode);
        return ESP_ERR_NOT_FOUND;
    }

    if (selected_candidate != NULL) {
        *selected_candidate = candidate;
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
    solar_os_ble_hid_close(connected_dev);

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
    ble_keyboard_candidate_t selected_candidate = {0};
    esp_err_t ret = run_keyboard_scan(mode, &selected_candidate);
    if (ret != ESP_OK) {
        return ret;
    }
    if (mode == BLE_KEYBOARD_SCAN_PAIRING && pairing_scan_stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mode == BLE_KEYBOARD_SCAN_RECONNECT) {
        portENTER_CRITICAL(&reconnect_task_lock);
        const bool stop_requested = reconnect_stop_requested;
        portEXIT_CRITICAL(&reconnect_task_lock);
        if (stop_requested) {
            restore_status_after_scan(mode);
            return ESP_ERR_INVALID_STATE;
        }
    }

    SOLAR_OS_LOGI(TAG,
             "connecting " SOLAR_OS_BLE_ADDRESS_FMT " addr_type=%s name=%s",
             SOLAR_OS_BLE_ADDRESS_ARGS(selected_candidate.bda),
             addr_type_name(selected_candidate.addr_type),
             selected_candidate.name[0] ? selected_candidate.name : "(none)");
    set_status(BLE_KEYBOARD_CONNECTING,
               "connecting %s",
               selected_candidate.name[0] ? selected_candidate.name : "keyboard");

    if (mode == BLE_KEYBOARD_SCAN_PAIRING) {
        ret = close_connected_keyboard_for_pairing();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return open_keyboard(selected_candidate.bda,
                         selected_candidate.addr_type,
                         selected_candidate.name,
                         mode == BLE_KEYBOARD_SCAN_PAIRING ? "pairing" :
                         mode == BLE_KEYBOARD_SCAN_RECONNECT ? "reconnecting" :
                         "connecting");
}

static void scan_task(void *arg)
{
    (void)arg;

    const esp_err_t ret = scan_and_open_keyboard(BLE_KEYBOARD_SCAN_PAIRING);
    if (pairing_scan_stop_requested) {
        pairing_scan_stop_requested = false;
        if (!connected) {
            set_status(BLE_KEYBOARD_IDLE, "pairing stopped");
        }
    }
    if (ret != ESP_OK && !connected) {
        schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
    }

    scan_task_handle = NULL;
    solar_os_task_delete_internal(NULL);
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
    if (solar_os_task_create_pinned_internal(scan_task,
                                             "ble_kbd_scan",
                                             6144,
                                             NULL,
                                             4,
                                             &scan_task_handle,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
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

static void defer_bond_forget(const ble_keyboard_peer_t *peer)
{
    if (peer == NULL || peer->magic != BLE_KEYBOARD_PEER_MAGIC) {
        return;
    }

    portENTER_CRITICAL(&reconnect_task_lock);
    memcpy(deferred_forget_bda, peer->bda, sizeof(deferred_forget_bda));
    deferred_forget_valid = true;
    portEXIT_CRITICAL(&reconnect_task_lock);
    SOLAR_OS_LOGI(TAG,
                  "deferring BLE bond removal until HID is closed");
}

static bool deferred_bond_forget_pending(void)
{
    portENTER_CRITICAL(&reconnect_task_lock);
    const bool pending = deferred_forget_valid;
    portEXIT_CRITICAL(&reconnect_task_lock);
    return pending;
}

static bool bond_forget_pending(void)
{
    portENTER_CRITICAL(&bond_remove_lock);
    const bool pending = bond_remove_pending;
    portEXIT_CRITICAL(&bond_remove_lock);
    return pending;
}

static bool forget_operation_pending(void)
{
    return reconnect_suppressed_for_forget ||
        deferred_bond_forget_pending() ||
        bond_forget_pending();
}

static void clear_deferred_bond_forget(void)
{
    portENTER_CRITICAL(&reconnect_task_lock);
    deferred_forget_valid = false;
    memset(deferred_forget_bda, 0, sizeof(deferred_forget_bda));
    portEXIT_CRITICAL(&reconnect_task_lock);
}

static esp_err_t bonded_peer_is_present(const uint8_t *bda, bool *present)
{
    if (!bda || !present) return ESP_ERR_INVALID_ARG;
    *present = false;
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc) return solar_os_ble_nimble_error(rc);
    for (int i = 0; i < count; ++i) {
        uint8_t address[6];
        solar_os_ble_nimble_display_address(address, &peers[i]);
        if (!memcmp(address, bda, 6)) *present = true;
    }
    return ESP_OK;
}

static void finish_forget_operation(esp_err_t result, bool bond_removed)
{
    clear_deferred_bond_forget();
    reconnect_suppressed_for_forget = false;

    if (result == ESP_OK) {
        set_status(BLE_KEYBOARD_IDLE, "forgot keyboard");
        SOLAR_OS_LOGI(TAG, "BLE keyboard bond and remembered state removed");
    } else {
        set_status(BLE_KEYBOARD_FAILED, "forget failed: %s", esp_err_to_name(result));
        SOLAR_OS_LOGW(TAG, "BLE keyboard forget failed: %s", esp_err_to_name(result));
    }

    if (pairing_retry_pending) {
        const esp_err_t pair_ret = start_pairing_scan_now();
        if (pair_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "deferred pairing start after forget failed: %s",
                          esp_err_to_name(pair_ret));
        }
    } else if (!bond_removed && remembered_peer_count() > 0U) {
        schedule_reconnect(BLE_KEYBOARD_RECONNECT_INITIAL_DELAY_MS);
    }
}

static esp_err_t complete_bond_forget(const uint8_t *bda, esp_err_t result)
{
    bool bond_removed = result == ESP_OK;
    if (!bond_removed) {
        bool still_bonded = true;
        const esp_err_t query_ret = bonded_peer_is_present(bda, &still_bonded);
        if (query_ret == ESP_OK && !still_bonded) {
            SOLAR_OS_LOGI(TAG,
                          "bond " SOLAR_OS_BLE_ADDRESS_FMT " is already absent; reconciling state",
                          SOLAR_OS_BLE_ADDRESS_ARGS(bda));
            bond_removed = true;
            result = ESP_OK;
        }
    }

    if (bond_removed) {
        const esp_err_t clear_ret = clear_remembered_peers();
        if (clear_ret != ESP_OK) {
            result = clear_ret;
        }
    }
    finish_forget_operation(result, bond_removed);
    return result;
}

static esp_err_t remove_deferred_bonds(void)
{
    ble_address_t bda = {0};

    portENTER_CRITICAL(&reconnect_task_lock);
    const bool deferred = deferred_forget_valid;
    if (deferred) {
        memcpy(bda, deferred_forget_bda, sizeof(bda));
    }
    portEXIT_CRITICAL(&reconnect_task_lock);
    if (!deferred) {
        return ESP_OK;
    }

    portENTER_CRITICAL(&bond_remove_lock);
    if (bond_remove_pending) {
        portEXIT_CRITICAL(&bond_remove_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bond_remove_pending = true;
    memcpy(bond_remove_bda, bda, sizeof(bond_remove_bda));
    portEXIT_CRITICAL(&bond_remove_lock);
    clear_deferred_bond_forget();

    /* NimBLE bond removal is synchronous. There is no persisted generic GATT
     * cache. Clear the remembered peer only after storage confirms removal. */
    const ble_keyboard_peer_t *peer = remembered_peer_for_bda(bda);
    ble_addr_t addr;
    solar_os_ble_nimble_address(&addr, bda, peer ? peer->addr_type : BLE_ADDR_RANDOM);
    addr.type &= 1;
    const esp_err_t remove_ret = solar_os_ble_nimble_error(ble_gap_unpair(&addr));
    portENTER_CRITICAL(&bond_remove_lock);
    bond_remove_pending = false;
    memset(bond_remove_bda, 0, sizeof(bond_remove_bda));
    portEXIT_CRITICAL(&bond_remove_lock);
    return complete_bond_forget(bda, remove_ret);
}

static esp_err_t complete_deferred_bond_forget(void)
{
    if (!deferred_bond_forget_pending()) {
        return ESP_OK;
    }

    if (!close_pending_open_attempt("forget", BLE_KEYBOARD_STALE_CLOSE_TIMEOUT_MS)) {
        finish_forget_operation(ESP_ERR_TIMEOUT, false);
        return ESP_ERR_TIMEOUT;
    }

    if (connected_dev != NULL) {
        while (close_done_sem != NULL &&
               xSemaphoreTake(close_done_sem, 0) == pdTRUE) {
        }
        const esp_err_t close_ret = solar_os_ble_hid_close(connected_dev);
        if (close_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "deferred forget keyboard close failed: %s",
                          esp_err_to_name(close_ret));
            finish_forget_operation(close_ret, false);
            return close_ret;
        } else if (close_done_sem != NULL &&
                   xSemaphoreTake(
                       close_done_sem,
                       pdMS_TO_TICKS(BLE_KEYBOARD_STALE_CLOSE_TIMEOUT_MS)) !=
                       pdTRUE) {
            SOLAR_OS_LOGW(TAG, "deferred forget keyboard close timeout");
            finish_forget_operation(ESP_ERR_TIMEOUT, false);
            return ESP_ERR_TIMEOUT;
        }
    }

    return remove_deferred_bonds();
}

static void reconnect_task(void *arg)
{
    const uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    uint32_t backoff_ms = BLE_KEYBOARD_RECONNECT_BACKOFF_INITIAL_MS;

    if (delay_ms > 0) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }

    while (initialized && remembered_peer_count() > 0 && !connected) {
        portENTER_CRITICAL(&reconnect_task_lock);
        const bool stop_requested = reconnect_stop_requested;
        portEXIT_CRITICAL(&reconnect_task_lock);
        if (stop_requested) {
            break;
        }

        if (scan_task_handle == NULL &&
            state != BLE_KEYBOARD_SCANNING &&
            state != BLE_KEYBOARD_CONNECTING &&
            state != BLE_KEYBOARD_PASSKEY) {
            const ble_keyboard_peer_t *peer = primary_remembered_peer();
            if (peer == NULL) {
                break;
            }
            portENTER_CRITICAL(&reconnect_task_lock);
            if (reconnect_stop_requested) {
                portEXIT_CRITICAL(&reconnect_task_lock);
                break;
            }
            reconnect_open_in_progress = true;
            portEXIT_CRITICAL(&reconnect_task_lock);

            SOLAR_OS_LOGI(TAG,
                          "reconnecting remembered keyboard " SOLAR_OS_BLE_ADDRESS_FMT,
                          SOLAR_OS_BLE_ADDRESS_ARGS(peer->bda));
            const esp_err_t ret = scan_and_open_keyboard(BLE_KEYBOARD_SCAN_RECONNECT);

            portENTER_CRITICAL(&reconnect_task_lock);
            reconnect_open_in_progress = false;
            const bool stop_after_open = reconnect_stop_requested;
            portEXIT_CRITICAL(&reconnect_task_lock);
            if (ret == ESP_OK) {
                backoff_ms = BLE_KEYBOARD_RECONNECT_BACKOFF_INITIAL_MS;
            } else if (ret != ESP_ERR_NOT_FOUND) {
                SOLAR_OS_LOGI(TAG,
                              "reconnect open failed; returning to scan: %s",
                              esp_err_to_name(ret));
            }
            if (stop_after_open) {
                break;
            }
            if (connected) {
                break;
            }

            if (backoff_ms < BLE_KEYBOARD_RECONNECT_BACKOFF_MAX_MS) {
                backoff_ms *= 2U;
                if (backoff_ms > BLE_KEYBOARD_RECONNECT_BACKOFF_MAX_MS) {
                    backoff_ms = BLE_KEYBOARD_RECONNECT_BACKOFF_MAX_MS;
                }
            }

            (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff_ms));
            continue;
        }

        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }

    portENTER_CRITICAL(&reconnect_task_lock);
    reconnect_task_handle = NULL;
    reconnect_stop_requested = false;
    reconnect_open_in_progress = false;
    portEXIT_CRITICAL(&reconnect_task_lock);

    (void)complete_deferred_bond_forget();
    if (pairing_retry_pending && !forget_operation_pending()) {
        const esp_err_t pair_ret = start_pairing_scan_now();
        if (pair_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG,
                          "deferred pairing start after reconnect stop failed: %s",
                          esp_err_to_name(pair_ret));
        }
    }
    solar_os_task_delete_internal(NULL);
}

static void schedule_reconnect(uint32_t delay_ms)
{
    if (!initialized || connected || remembered_peer_count() == 0 || reconnect_is_suppressed()) {
        return;
    }
    portENTER_CRITICAL(&reconnect_task_lock);
    TaskHandle_t active_reconnect_task = reconnect_task_handle;
    const bool reconnect_stopping = reconnect_stop_requested;
    portEXIT_CRITICAL(&reconnect_task_lock);
    if (active_reconnect_task != NULL) {
        if (!reconnect_stopping) {
            xTaskNotifyGive(active_reconnect_task);
        }
        return;
    }

    portENTER_CRITICAL(&reconnect_task_lock);
    reconnect_stop_requested = false;
    reconnect_open_in_progress = false;
    portEXIT_CRITICAL(&reconnect_task_lock);
    if (solar_os_task_create_pinned_internal(reconnect_task,
                                             "ble_kbd_reconn",
                                             4096,
                                             (void *)(uintptr_t)delay_ms,
                                             3,
                                             &reconnect_task_handle,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        reconnect_task_handle = NULL;
        SOLAR_OS_LOGW(TAG, "reconnect task failed");
    }
}

esp_err_t solar_os_ble_keyboard_start_pairing(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    pairing_scan_stop_requested = false;
    if (forget_operation_pending()) {
        pairing_retry_pending = true;
        set_status(BLE_KEYBOARD_PAIRING_PENDING, "pairing after forget");
        SOLAR_OS_LOGI(TAG, "pairing waits for keyboard forget completion");
        return ESP_OK;
    }

    const bool reconnect_stopped = stop_reconnect_task("pairing", 50U);

    if (!reconnect_stopped) {
        request_pairing_after_pending_connect("pairing");
        return ESP_OK;
    }

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

esp_err_t solar_os_ble_backend_prepare_sleep(uint32_t timeout_ms)
{
    if (!initialized) return ESP_OK;
    if (forget_operation_pending()) return ESP_ERR_INVALID_STATE;
    reconnect_suppressed_for_sleep = true;
    pairing_retry_pending = false;
    reconnect_suppressed_for_pairing = false;
    solar_os_ble_hid_suspend();
    if (!stop_reconnect_task("sleep", timeout_ms)) return ESP_ERR_NOT_FINISHED;
    stop_scan_task_for_sleep(timeout_ms);
    if (scan_task_handle) return ESP_ERR_NOT_FINISHED;
    if (connected_dev) (void)solar_os_ble_hid_close(connected_dev);
    TickType_t start = xTaskGetTickCount();
    while (!solar_os_ble_hid_idle() || !solar_os_ble_nimble_client_idle()) {
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(timeout_ms)) return ESP_ERR_NOT_FINISHED;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_RETURN_ON_ERROR(nimble_port_stop(), TAG, "NimBLE stop failed");
    xSemaphoreTake(host_stopped, portMAX_DELAY);
    ESP_RETURN_ON_ERROR(solar_os_ble_hid_deinit(), TAG, "HID stop failed");
    solar_os_ble_nimble_host_stopped();
    ESP_RETURN_ON_ERROR(nimble_port_deinit(), TAG, "NimBLE deinit failed");
    clear_runtime_connection_state("sleep");
    solar_os_ble_service_reset("sleep");
    solar_os_ble_backend_reset();
    initialized = false;
    set_status(BLE_KEYBOARD_IDLE, "sleep");
    return ESP_OK;
}

bool solar_os_ble_backend_sleep_prepare_ready(void)
{
    if (!initialized) {
        return true;
    }

    portENTER_CRITICAL(&reconnect_task_lock);
    const bool reconnect_stopped = reconnect_task_handle == NULL;
    portEXIT_CRITICAL(&reconnect_task_lock);
    return reconnect_stopped && !scan_task_handle && !forget_operation_pending() &&
        solar_os_ble_hid_idle() && solar_os_ble_nimble_client_idle();
}

void solar_os_ble_backend_resume(void)
{
    if (!solar_os_ble_keyboard_enabled_for_current_boot()) {
        return;
    }

    reconnect_suppressed_for_sleep = false;
    reconnect_suppressed_for_pairing = false;
    pairing_retry_pending = false;
    pairing_scan_stop_requested = false;

    /* The general service already serializes this lifecycle transition. */
    const esp_err_t ret = solar_os_ble_backend_init();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "resume: BLE init failed: %s", esp_err_to_name(ret));
        set_status(BLE_KEYBOARD_FAILED, "resume failed");
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(BLE_KEYBOARD_RESUME_RECONNECT_DELAY_MS));

    if (!initialized || connected || remembered_peer_count() == 0) {
        return;
    }

    keyboard_report_state_reset(false);
    schedule_reconnect(0);
}

static esp_err_t forget_remembered_keyboard(void)
{
    if (forget_operation_pending()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (remembered_peer_count() == 0U) {
        set_status(BLE_KEYBOARD_IDLE, "no remembered keyboard");
        return ESP_OK;
    }

    reconnect_suppressed_for_forget = true;
    defer_bond_forget(&remembered_peers[0]);
    set_status(BLE_KEYBOARD_PAIRING_PENDING, "forget pending");
    const bool reconnect_stopped = stop_reconnect_task("forget", 50U);

    if (reconnect_stopped) {
        return complete_deferred_bond_forget();
    }

    return ESP_OK;
}

esp_err_t solar_os_ble_keyboard_forget(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return forget_remembered_keyboard();
}

/* Existing keyboard callers share the general BLE service lifecycle. The
 * keyboard policy and shared NimBLE lifecycle stay together in this
 * backend implementation until a different host is evaluated. */
esp_err_t solar_os_ble_keyboard_init(void)
{
    return solar_os_ble_init();
}

esp_err_t solar_os_ble_keyboard_scan(solar_os_ble_keyboard_scan_result_t *results,
                                     size_t max_results, size_t *found)
{
    return solar_os_ble_scan(results, max_results, found);
}

esp_err_t solar_os_ble_keyboard_prepare_sleep(uint32_t timeout_ms)
{
    return solar_os_ble_prepare_sleep(timeout_ms);
}

bool solar_os_ble_keyboard_sleep_prepare_ready(void)
{
    return solar_os_ble_sleep_prepare_ready();
}

void solar_os_ble_keyboard_resume(void)
{
    solar_os_ble_resume();
}
