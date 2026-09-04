#include "solar_os_wifi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "solar_os_identity.h"
#include "solar_os_log.h"
#include "solar_os_wifi_repeater.h"

static const char *TAG = "solar_os_wifi";

#define SOLAR_OS_WIFI_DEFAULT_AP_SSID "SolarOS-sol"
#define SOLAR_OS_WIFI_DEFAULT_AP_CHANNEL 6
#define SOLAR_OS_WIFI_DEFAULT_AP_MAX_CONNECTIONS 4
#define WIFI_AP_NVS_NAMESPACE "wifi_ap"
#define WIFI_AP_NVS_SSID_KEY "ssid"
#define WIFI_AP_NVS_PASSWORD_KEY "password"
#define WIFI_AP_NVS_AUTH_KEY "auth"
#define WIFI_NAT_NVS_NAMESPACE "wifi_nat"
#define WIFI_NAT_NVS_ENABLED_KEY "enabled"
#define WIFI_POLICY_NVS_NAMESPACE "wifi"
#define WIFI_POLICY_NVS_ENABLED_KEY "enabled"
#define WIFI_STA_NVS_NAMESPACE "wifi_sta"
#define WIFI_STA_NVS_COUNT_KEY "count"
#define WIFI_STA_NVS_SSID_PREFIX "ssid"
#define WIFI_STA_NVS_PASSWORD_PREFIX "pass"
#define WIFI_REPEATER_RECONNECT_INITIAL_MS 1000U
#define WIFI_REPEATER_RECONNECT_MAX_MS 30000U
#define WIFI_REPEATER_AP_SETTLE_MS 100U
#define WIFI_REPEATER_AP_START_TIMEOUT_MS 1500U

typedef struct {
    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
    char password[SOLAR_OS_WIFI_PASSWORD_MAX];
} wifi_profile_t;

static SemaphoreHandle_t wifi_mutex;
static esp_netif_t *wifi_sta_netif;
static esp_netif_t *wifi_ap_netif;
static bool wifi_initialized;
/* Freeze the current-boot policy before shell changes update the next boot. */
static bool wifi_boot_policy_loaded;
static bool wifi_enabled_for_current_boot = true;
static bool wifi_enabled_for_next_boot = true;
static bool wifi_started;
static bool wifi_sta_enabled;
static bool wifi_connected;
static bool wifi_has_ip;
static bool wifi_has_saved_config;
static bool wifi_has_saved_ap_config;
static bool wifi_nat_enabled;
static bool wifi_nat_active;
static bool wifi_repeater_starting;
static bool wifi_ap_enabled;
static bool wifi_ap_running;
static bool wifi_connectionless_active;
static bool wifi_connectionless_channel_auto;
static bool wifi_suspended;
static bool wifi_sleep_was_started;
static bool wifi_sleep_sta_enabled;
static bool wifi_sleep_ap_enabled;
static bool wifi_sleep_reconnect_sta;
static solar_os_wifi_state_t wifi_state = SOLAR_OS_WIFI_STATE_OFF;
static char wifi_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
static char wifi_saved_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
static EXT_RAM_BSS_ATTR wifi_profile_t wifi_profiles[SOLAR_OS_WIFI_PROFILE_MAX];
static size_t wifi_profile_count;
static char wifi_saved_ap_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
static char wifi_saved_ap_password[SOLAR_OS_WIFI_PASSWORD_MAX];
static char wifi_saved_ap_auth[SOLAR_OS_WIFI_AUTH_MAX];
static char wifi_ip[16];
static char wifi_gateway[16];
static char wifi_netmask[16];
static char wifi_ap_ssid[SOLAR_OS_WIFI_SSID_MAX + 1];
static char wifi_ap_auth[SOLAR_OS_WIFI_AUTH_MAX];
static char wifi_ap_ip[16];
static int8_t wifi_rssi;
static uint8_t wifi_channel;
static uint8_t wifi_disconnect_reason;
static uint8_t wifi_ap_channel;
static uint8_t wifi_ap_station_count;
static uint8_t wifi_ap_max_connections;
static uint8_t wifi_connectionless_channel;
static esp_err_t wifi_nat_last_error;
static esp_timer_handle_t wifi_repeater_reconnect_timer;
static uint32_t wifi_repeater_reconnect_delay_ms = WIFI_REPEATER_RECONNECT_INITIAL_MS;
static char wifi_connectionless_owner[SOLAR_OS_WIFI_CONNECTIONLESS_OWNER_MAX];
static char wifi_latency_owner[SOLAR_OS_WIFI_LATENCY_OWNER_MAX];

static void wifi_set_started_state(bool started);
static esp_err_t wifi_update_ap_dns_from_sta(void);
static void wifi_repeater_schedule_reconnect(void);
static void wifi_lock(void);
static void wifi_unlock(void);

static esp_err_t wifi_wait_for_ap_running(void)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_REPEATER_AP_SETTLE_MS));
    for (uint32_t elapsed = WIFI_REPEATER_AP_SETTLE_MS;
         elapsed < WIFI_REPEATER_AP_START_TIMEOUT_MS;
         elapsed += 20U) {
        wifi_lock();
        const bool running = wifi_ap_running;
        wifi_unlock();
        if (running) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(20U));
    }
    return ESP_ERR_TIMEOUT;
}

static esp_err_t wifi_load_boot_policy(void)
{
    if (wifi_boot_policy_loaded) {
        return ESP_OK;
    }

    wifi_enabled_for_current_boot = true;
    wifi_enabled_for_next_boot = true;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_POLICY_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        wifi_boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        wifi_boot_policy_loaded = true;
        return ret;
    }

    uint8_t stored = 1U;
    ret = nvs_get_u8(nvs, WIFI_POLICY_NVS_ENABLED_KEY, &stored);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        wifi_boot_policy_loaded = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        wifi_boot_policy_loaded = true;
        return ret;
    }

    wifi_enabled_for_current_boot = stored != 0U;
    wifi_enabled_for_next_boot = wifi_enabled_for_current_boot;
    wifi_boot_policy_loaded = true;
    return ESP_OK;
}

bool solar_os_wifi_enabled_for_current_boot(void)
{
    const esp_err_t ret = wifi_load_boot_policy();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "load Wi-Fi boot policy failed; defaulting to enabled: %s",
                      esp_err_to_name(ret));
    }
    return wifi_enabled_for_current_boot;
}

bool solar_os_wifi_enabled_for_next_boot(void)
{
    (void)solar_os_wifi_enabled_for_current_boot();
    return wifi_enabled_for_next_boot;
}

esp_err_t solar_os_wifi_set_enabled_for_next_boot(bool enabled)
{
    (void)solar_os_wifi_enabled_for_current_boot();

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_POLICY_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, WIFI_POLICY_NVS_ENABLED_KEY, enabled ? 1U : 0U);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        wifi_enabled_for_next_boot = enabled;
    }
    return ret;
}

static void wifi_lock(void)
{
    if (wifi_mutex != NULL) {
        xSemaphoreTake(wifi_mutex, portMAX_DELAY);
    }
}

static void wifi_unlock(void)
{
    if (wifi_mutex != NULL) {
        xSemaphoreGive(wifi_mutex);
    }
}

static void wifi_repeater_cancel_reconnect(void)
{
    if (wifi_repeater_reconnect_timer != NULL &&
        esp_timer_is_active(wifi_repeater_reconnect_timer)) {
        (void)esp_timer_stop(wifi_repeater_reconnect_timer);
    }
    wifi_lock();
    wifi_repeater_reconnect_delay_ms = WIFI_REPEATER_RECONNECT_INITIAL_MS;
    wifi_unlock();
}

static void wifi_repeater_reconnect_callback(void *argument)
{
    (void)argument;

    wifi_lock();
    const bool should_connect = solar_os_wifi_repeater_is_enabled() &&
        wifi_started && wifi_sta_enabled && !wifi_suspended && !wifi_connected &&
        wifi_state == SOLAR_OS_WIFI_STATE_DISCONNECTED;
    if (should_connect) {
        wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
    }
    wifi_unlock();
    if (!should_connect) {
        return;
    }

    const esp_err_t error = esp_wifi_connect();
    if (error != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_DISCONNECTED;
        wifi_unlock();
        wifi_repeater_schedule_reconnect();
    }
}

static void wifi_repeater_schedule_reconnect(void)
{
    if (!solar_os_wifi_repeater_is_enabled() || wifi_repeater_reconnect_timer == NULL) {
        return;
    }
    if (esp_timer_is_active(wifi_repeater_reconnect_timer)) {
        (void)esp_timer_stop(wifi_repeater_reconnect_timer);
    }
    wifi_lock();
    const uint32_t delay_ms = wifi_repeater_reconnect_delay_ms;
    if (wifi_repeater_reconnect_delay_ms < WIFI_REPEATER_RECONNECT_MAX_MS) {
        wifi_repeater_reconnect_delay_ms *= 2U;
        if (wifi_repeater_reconnect_delay_ms > WIFI_REPEATER_RECONNECT_MAX_MS) {
            wifi_repeater_reconnect_delay_ms = WIFI_REPEATER_RECONNECT_MAX_MS;
        }
    }
    wifi_unlock();
    (void)esp_timer_start_once(wifi_repeater_reconnect_timer,
                               (uint64_t)delay_ms * 1000ULL);
}

static wifi_ps_type_t wifi_power_save_mode_locked(void)
{
    return wifi_connectionless_active || wifi_latency_owner[0] != '\0' ||
        solar_os_wifi_repeater_is_enabled() ?
        WIFI_PS_NONE : WIFI_PS_MAX_MODEM;
}

static wifi_ps_type_t wifi_power_save_mode(void)
{
    wifi_lock();
    const wifi_ps_type_t mode = wifi_power_save_mode_locked();
    wifi_unlock();
    return mode;
}

static bool wifi_connectionless_fixed(void)
{
    bool fixed = false;
    wifi_lock();
    fixed = wifi_connectionless_active && !wifi_connectionless_channel_auto;
    wifi_unlock();
    return fixed;
}

static void wifi_copy_ssid(char *dest, size_t dest_len, const uint8_t *ssid, size_t ssid_len)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }

    size_t copy_len = 0;
    while (copy_len < ssid_len && ssid[copy_len] != '\0') {
        copy_len++;
    }
    if (copy_len >= dest_len) {
        copy_len = dest_len - 1;
    }

    memcpy(dest, ssid, copy_len);
    dest[copy_len] = '\0';
}

static void wifi_format_ip(const esp_ip4_addr_t *ip, char *dest, size_t dest_len)
{
    if (dest == NULL || dest_len == 0) {
        return;
    }

    if (ip == NULL || ip->addr == 0) {
        strlcpy(dest, "0.0.0.0", dest_len);
        return;
    }

    snprintf(dest, dest_len, IPSTR, IP2STR(ip));
}

static const char *wifi_auth_name(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa/wpa2";
    case WIFI_AUTH_ENTERPRISE:
        return "wpa2-eap";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2/wpa3";
    case WIFI_AUTH_WAPI_PSK:
        return "wapi";
    case WIFI_AUTH_OWE:
        return "owe";
    case WIFI_AUTH_WPA3_ENT_192:
        return "wpa3-ent";
    case WIFI_AUTH_DPP:
        return "dpp";
    case WIFI_AUTH_WPA3_ENTERPRISE:
        return "wpa3-eap";
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
        return "wpa2/3-eap";
    case WIFI_AUTH_WPA_ENTERPRISE:
        return "wpa-eap";
    default:
        return "unknown";
    }
}

static bool wifi_auth_from_name(const char *name, wifi_auth_mode_t *authmode)
{
    if (name == NULL || name[0] == '\0' || strcmp(name, "wpa2") == 0) {
        *authmode = WIFI_AUTH_WPA2_PSK;
        return true;
    }
    if (strcmp(name, "open") == 0) {
        *authmode = WIFI_AUTH_OPEN;
        return true;
    }
    if (strcmp(name, "wpa") == 0) {
        *authmode = WIFI_AUTH_WPA_PSK;
        return true;
    }
    if (strcmp(name, "wpa/wpa2") == 0 || strcmp(name, "wpa2/wpa") == 0) {
        *authmode = WIFI_AUTH_WPA_WPA2_PSK;
        return true;
    }
    if (strcmp(name, "wep") == 0) {
        *authmode = WIFI_AUTH_WEP;
        return true;
    }

    return false;
}

static esp_err_t wifi_validate_ap_settings(const char *ssid,
                                           const char *password,
                                           const char *auth,
                                           wifi_auth_mode_t *authmode)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > SOLAR_OS_WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }

    const size_t password_len = strlen(password);
    if (password_len >= SOLAR_OS_WIFI_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_auth_mode_t selected_auth = password_len == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    if (auth != NULL && auth[0] != '\0') {
        if (!wifi_auth_from_name(auth, &selected_auth)) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (selected_auth == WIFI_AUTH_WEP) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (selected_auth == WIFI_AUTH_OPEN && password_len != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (selected_auth != WIFI_AUTH_OPEN && password_len < 8) {
        return ESP_ERR_INVALID_ARG;
    }

    if (authmode != NULL) {
        *authmode = selected_auth;
    }
    return ESP_OK;
}

static esp_err_t wifi_validate_station_settings(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > SOLAR_OS_WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password != NULL && strlen(password) >= SOLAR_OS_WIFI_PASSWORD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void wifi_sta_nvs_key(char *key, size_t key_len, const char *prefix, size_t index)
{
    if (key == NULL || key_len == 0) {
        return;
    }
    snprintf(key, key_len, "%s%u", prefix, (unsigned)index);
}

static void wifi_refresh_saved_config_locked(void)
{
    if (wifi_profile_count > 0) {
        wifi_has_saved_config = true;
        strlcpy(wifi_saved_ssid, wifi_profiles[0].ssid, sizeof(wifi_saved_ssid));
    } else {
        wifi_has_saved_config = false;
        wifi_saved_ssid[0] = '\0';
    }
}

static void wifi_clear_profiles_locked(void)
{
    memset(wifi_profiles, 0, sizeof(wifi_profiles));
    wifi_profile_count = 0;
    wifi_refresh_saved_config_locked();
}

static int wifi_find_profile_index_locked(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < wifi_profile_count; i++) {
        if (strcmp(wifi_profiles[i].ssid, ssid) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static esp_err_t wifi_save_profiles(void)
{
    wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;

    wifi_lock();
    count = wifi_profile_count;
    memcpy(profiles, wifi_profiles, sizeof(profiles));
    wifi_unlock();

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_STA_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, WIFI_STA_NVS_COUNT_KEY, (uint8_t)count);
    for (size_t i = 0; ret == ESP_OK && i < SOLAR_OS_WIFI_PROFILE_MAX; i++) {
        char ssid_key[12];
        char password_key[12];
        wifi_sta_nvs_key(ssid_key, sizeof(ssid_key), WIFI_STA_NVS_SSID_PREFIX, i);
        wifi_sta_nvs_key(password_key, sizeof(password_key), WIFI_STA_NVS_PASSWORD_PREFIX, i);
        if (i < count) {
            ret = nvs_set_str(nvs, ssid_key, profiles[i].ssid);
            if (ret == ESP_OK) {
                ret = nvs_set_str(nvs, password_key, profiles[i].password);
            }
        } else {
            esp_err_t erase_ret = nvs_erase_key(nvs, ssid_key);
            if (erase_ret != ESP_OK && erase_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = erase_ret;
                break;
            }
            erase_ret = nvs_erase_key(nvs, password_key);
            if (erase_ret != ESP_OK && erase_ret != ESP_ERR_NVS_NOT_FOUND) {
                ret = erase_ret;
                break;
            }
        }
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t wifi_load_profiles(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_STA_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        wifi_lock();
        wifi_clear_profiles_locked();
        wifi_unlock();
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t stored_count = 0;
    ret = nvs_get_u8(nvs, WIFI_STA_NVS_COUNT_KEY, &stored_count);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        stored_count = 0;
        ret = ESP_OK;
    }

    wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX] = {0};
    size_t count = 0;
    const size_t max_count = stored_count > SOLAR_OS_WIFI_PROFILE_MAX ?
        SOLAR_OS_WIFI_PROFILE_MAX :
        stored_count;

    for (size_t i = 0; ret == ESP_OK && i < max_count; i++) {
        char ssid_key[12];
        char password_key[12];
        char ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
        char password[SOLAR_OS_WIFI_PASSWORD_MAX] = {0};
        size_t len = sizeof(ssid);
        wifi_sta_nvs_key(ssid_key, sizeof(ssid_key), WIFI_STA_NVS_SSID_PREFIX, i);
        wifi_sta_nvs_key(password_key, sizeof(password_key), WIFI_STA_NVS_PASSWORD_PREFIX, i);

        esp_err_t item_ret = nvs_get_str(nvs, ssid_key, ssid, &len);
        if (item_ret == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (item_ret != ESP_OK) {
            ret = item_ret;
            break;
        }

        len = sizeof(password);
        item_ret = nvs_get_str(nvs, password_key, password, &len);
        if (item_ret == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
        } else if (item_ret != ESP_OK) {
            ret = item_ret;
            break;
        }

        if (wifi_validate_station_settings(ssid, password) == ESP_OK) {
            strlcpy(profiles[count].ssid, ssid, sizeof(profiles[count].ssid));
            strlcpy(profiles[count].password, password, sizeof(profiles[count].password));
            count++;
        }
    }
    nvs_close(nvs);

    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    memset(wifi_profiles, 0, sizeof(wifi_profiles));
    memcpy(wifi_profiles, profiles, sizeof(profiles));
    wifi_profile_count = count;
    wifi_refresh_saved_config_locked();
    wifi_unlock();
    return ESP_OK;
}

static esp_err_t wifi_program_station_config(const wifi_profile_t *profile)
{
    wifi_config_t config = {0};
    if (profile != NULL) {
        memcpy(config.sta.ssid, profile->ssid, strlen(profile->ssid));
        memcpy(config.sta.password, profile->password, strlen(profile->password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.failure_retry_cnt = 3;
    }
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static esp_err_t wifi_migrate_legacy_station_config(void)
{
    wifi_lock();
    const bool has_profiles = wifi_profile_count > 0;
    wifi_unlock();
    if (has_profiles) {
        return ESP_OK;
    }

    wifi_config_t config = {0};
    esp_err_t ret = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
    char password[SOLAR_OS_WIFI_PASSWORD_MAX] = {0};
    wifi_copy_ssid(ssid, sizeof(ssid), config.sta.ssid, sizeof(config.sta.ssid));
    wifi_copy_ssid(password, sizeof(password), config.sta.password, sizeof(config.sta.password));
    if (ssid[0] == '\0' || wifi_validate_station_settings(ssid, password) != ESP_OK) {
        return ESP_OK;
    }

    wifi_lock();
    if (wifi_profile_count == 0) {
        strlcpy(wifi_profiles[0].ssid, ssid, sizeof(wifi_profiles[0].ssid));
        strlcpy(wifi_profiles[0].password, password, sizeof(wifi_profiles[0].password));
        wifi_profile_count = 1;
        wifi_refresh_saved_config_locked();
    }
    wifi_unlock();

    ret = wifi_save_profiles();
    if (ret == ESP_OK) {
        SOLAR_OS_LOGI(TAG, "migrated saved Wi-Fi network %s", ssid);
    }
    return ret;
}

static esp_err_t wifi_upsert_profile(const char *ssid, const char *password)
{
    if (password == NULL) {
        password = "";
    }

    wifi_profile_t profile = {0};
    strlcpy(profile.ssid, ssid, sizeof(profile.ssid));
    strlcpy(profile.password, password, sizeof(profile.password));

    wifi_lock();
    int existing = wifi_find_profile_index_locked(ssid);
    if (existing == 0 && strcmp(wifi_profiles[0].password, password) == 0) {
        wifi_refresh_saved_config_locked();
        wifi_unlock();
        return ESP_OK;
    }
    if (existing < 0 && wifi_profile_count >= SOLAR_OS_WIFI_PROFILE_MAX) {
        existing = (int)wifi_profile_count - 1;
    }
    if (existing > 0) {
        memmove(&wifi_profiles[1],
                &wifi_profiles[0],
                (size_t)existing * sizeof(wifi_profiles[0]));
    } else if (existing < 0 && wifi_profile_count > 0) {
        memmove(&wifi_profiles[1],
                &wifi_profiles[0],
                wifi_profile_count * sizeof(wifi_profiles[0]));
    }
    wifi_profiles[0] = profile;
    if (existing < 0 && wifi_profile_count < SOLAR_OS_WIFI_PROFILE_MAX) {
        wifi_profile_count++;
    }
    wifi_refresh_saved_config_locked();
    wifi_unlock();

    return wifi_save_profiles();
}

static esp_err_t wifi_select_saved_profile(wifi_profile_t *selected)
{
    if (selected == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_profile_t profiles[SOLAR_OS_WIFI_PROFILE_MAX];
    size_t count = 0;

    wifi_lock();
    count = wifi_profile_count;
    memcpy(profiles, wifi_profiles, sizeof(profiles));
    wifi_unlock();

    if (count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (count == 1) {
        *selected = profiles[0];
        return ESP_OK;
    }

    wifi_lock();
    const solar_os_wifi_state_t previous_state = wifi_state;
    wifi_state = SOLAR_OS_WIFI_STATE_SCANNING;
    wifi_unlock();

    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = previous_state;
        wifi_unlock();
        *selected = profiles[0];
        return ESP_OK;
    }

    uint16_t record_count = SOLAR_OS_WIFI_SCAN_MAX_RESULTS;
    wifi_ap_record_t records[SOLAR_OS_WIFI_SCAN_MAX_RESULTS] = {0};
    ret = esp_wifi_scan_get_ap_records(&record_count, records);

    wifi_lock();
    wifi_state = previous_state;
    wifi_unlock();

    if (ret != ESP_OK) {
        *selected = profiles[0];
        return ESP_OK;
    }

    int best_index = -1;
    int8_t best_rssi = INT8_MIN;
    for (uint16_t record_index = 0; record_index < record_count; record_index++) {
        char ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
        wifi_copy_ssid(ssid, sizeof(ssid), records[record_index].ssid, sizeof(records[record_index].ssid));
        if (ssid[0] == '\0') {
            continue;
        }
        for (size_t profile_index = 0; profile_index < count; profile_index++) {
            if (strcmp(profiles[profile_index].ssid, ssid) == 0 &&
                (best_index < 0 || records[record_index].rssi > best_rssi)) {
                best_index = (int)profile_index;
                best_rssi = records[record_index].rssi;
            }
        }
    }

    *selected = profiles[best_index >= 0 ? (size_t)best_index : 0];
    return ESP_OK;
}

static void wifi_clear_saved_ap_config_locked(void)
{
    wifi_has_saved_ap_config = false;
    wifi_saved_ap_ssid[0] = '\0';
    wifi_saved_ap_password[0] = '\0';
    wifi_saved_ap_auth[0] = '\0';
}

static esp_err_t wifi_load_saved_ap_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_AP_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        wifi_lock();
        wifi_clear_saved_ap_config_locked();
        wifi_unlock();
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    char ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
    char password[SOLAR_OS_WIFI_PASSWORD_MAX] = {0};
    char auth[SOLAR_OS_WIFI_AUTH_MAX] = {0};
    size_t len = sizeof(ssid);
    ret = nvs_get_str(nvs, WIFI_AP_NVS_SSID_KEY, ssid, &len);
    if (ret == ESP_OK) {
        len = sizeof(password);
        esp_err_t password_ret = nvs_get_str(nvs, WIFI_AP_NVS_PASSWORD_KEY, password, &len);
        if (password_ret == ESP_ERR_NVS_NOT_FOUND) {
            password[0] = '\0';
        } else if (password_ret != ESP_OK) {
            ret = password_ret;
        }
    }
    if (ret == ESP_OK) {
        len = sizeof(auth);
        esp_err_t auth_ret = nvs_get_str(nvs, WIFI_AP_NVS_AUTH_KEY, auth, &len);
        if (auth_ret == ESP_ERR_NVS_NOT_FOUND) {
            strlcpy(auth, password[0] == '\0' ? "open" : "wpa2", sizeof(auth));
        } else if (auth_ret != ESP_OK) {
            ret = auth_ret;
        }
    }
    nvs_close(nvs);

    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    if (ret == ESP_OK) {
        ret = wifi_validate_ap_settings(ssid, password, auth, &authmode);
    }

    wifi_lock();
    if (ret == ESP_OK) {
        wifi_has_saved_ap_config = true;
        strlcpy(wifi_saved_ap_ssid, ssid, sizeof(wifi_saved_ap_ssid));
        strlcpy(wifi_saved_ap_password, password, sizeof(wifi_saved_ap_password));
        strlcpy(wifi_saved_ap_auth, wifi_auth_name(authmode), sizeof(wifi_saved_ap_auth));
    } else {
        wifi_clear_saved_ap_config_locked();
    }
    wifi_unlock();

    return ret == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : ret;
}

static esp_err_t wifi_save_ap_config(const char *ssid,
                                     const char *password,
                                     wifi_auth_mode_t authmode)
{
    if (password == NULL) {
        password = "";
    }

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_AP_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_str(nvs, WIFI_AP_NVS_SSID_KEY, ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, WIFI_AP_NVS_PASSWORD_KEY, password);
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs, WIFI_AP_NVS_AUTH_KEY, wifi_auth_name(authmode));
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        wifi_lock();
        wifi_has_saved_ap_config = true;
        strlcpy(wifi_saved_ap_ssid, ssid, sizeof(wifi_saved_ap_ssid));
        strlcpy(wifi_saved_ap_password, password, sizeof(wifi_saved_ap_password));
        strlcpy(wifi_saved_ap_auth, wifi_auth_name(authmode), sizeof(wifi_saved_ap_auth));
        wifi_unlock();
    }
    return ret;
}

static esp_err_t wifi_load_nat_config(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NAT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        wifi_lock();
        wifi_nat_enabled = false;
        wifi_nat_last_error = ESP_OK;
        wifi_unlock();
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t enabled = 0;
    ret = nvs_get_u8(nvs, WIFI_NAT_NVS_ENABLED_KEY, &enabled);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        enabled = 0;
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        wifi_lock();
        wifi_nat_enabled = enabled != 0;
        wifi_nat_last_error = ESP_OK;
        wifi_unlock();
    }
    return ret;
}

static esp_err_t wifi_save_nat_config(bool enabled)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(WIFI_NAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = nvs_set_u8(nvs, WIFI_NAT_NVS_ENABLED_KEY, enabled ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t wifi_apply_nat(void)
{
    bool nat_active = false;
    bool should_enable = false;

    wifi_lock();
    nat_active = wifi_nat_active;
    should_enable = wifi_nat_enabled &&
        !wifi_repeater_starting &&
        !solar_os_wifi_repeater_is_enabled() &&
        wifi_sta_enabled &&
        wifi_connected &&
        wifi_has_ip &&
        wifi_ap_enabled &&
        wifi_ap_running;
    wifi_unlock();

    if (!should_enable && !nat_active) {
        wifi_lock();
        wifi_nat_active = false;
        wifi_nat_last_error = ESP_OK;
        wifi_unlock();
        return ESP_OK;
    }

    if (wifi_ap_netif == NULL) {
        wifi_lock();
        wifi_nat_active = false;
        wifi_nat_last_error = should_enable ? ESP_ERR_INVALID_STATE : ESP_OK;
        wifi_unlock();
        return should_enable ? ESP_ERR_INVALID_STATE : ESP_OK;
    }

    if (should_enable && !nat_active) {
        esp_err_t dns_ret = wifi_update_ap_dns_from_sta();
        if (dns_ret != ESP_OK) {
            SOLAR_OS_LOGW(TAG, "AP DHCP DNS preparation failed before NAT: %s", esp_err_to_name(dns_ret));
        }
        const esp_err_t ret = esp_netif_napt_enable(wifi_ap_netif);
        wifi_lock();
        wifi_nat_active = ret == ESP_OK;
        wifi_nat_last_error = ret;
        wifi_unlock();
        if (ret == ESP_OK) {
            SOLAR_OS_LOGI(TAG, "NAT enabled on AP interface");
        } else if (ret != ESP_FAIL) {
            SOLAR_OS_LOGW(TAG, "NAT enable failed: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    if (!should_enable && nat_active) {
        const esp_err_t ret = esp_netif_napt_disable(wifi_ap_netif);
        wifi_lock();
        wifi_nat_active = false;
        wifi_nat_last_error = ESP_OK;
        wifi_unlock();
        if (ret == ESP_OK) {
            SOLAR_OS_LOGI(TAG, "NAT disabled on AP interface");
        }
        return ret == ESP_FAIL ? ESP_OK : ret;
    }

    return ESP_OK;
}

static esp_err_t wifi_update_ap_dns_from_sta(void)
{
    if (wifi_sta_netif == NULL || wifi_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_netif_dns_info_t dns = {0};
    esp_err_t ret = esp_netif_get_dns_info(wifi_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (ret != ESP_OK || dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) {
        return ret == ESP_OK ? ESP_ERR_NOT_FOUND : ret;
    }

    bool dns_matches = false;
    bool offer_dns = false;
    esp_netif_dns_info_t ap_dns = {0};
    ret = esp_netif_get_dns_info(wifi_ap_netif, ESP_NETIF_DNS_MAIN, &ap_dns);
    if (ret == ESP_OK &&
        ap_dns.ip.type == ESP_IPADDR_TYPE_V4 &&
        ap_dns.ip.u_addr.ip4.addr == dns.ip.u_addr.ip4.addr) {
        dns_matches = true;
    }

    uint8_t dhcp_offer_dns = 0;
    ret = esp_netif_dhcps_option(wifi_ap_netif,
                                 ESP_NETIF_OP_GET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &dhcp_offer_dns,
                                 sizeof(dhcp_offer_dns));
    if (ret == ESP_OK) {
        offer_dns = dhcp_offer_dns != 0;
    }

    if (dns_matches && offer_dns) {
        return ESP_OK;
    }

    esp_netif_dhcp_status_t dhcps_status = ESP_NETIF_DHCP_STOPPED;
    ret = esp_netif_dhcps_get_status(wifi_ap_netif, &dhcps_status);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "AP DHCP status failed before DNS update: %s", esp_err_to_name(ret));
        return ret;
    }

    const bool restart_dhcps = dhcps_status == ESP_NETIF_DHCP_STARTED;
    if (restart_dhcps) {
        ret = esp_netif_dhcps_stop(wifi_ap_netif);
        if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            SOLAR_OS_LOGW(TAG, "AP DHCP stop failed before DNS update: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    ret = esp_netif_set_dns_info(wifi_ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "AP DNS update failed: %s", esp_err_to_name(ret));
        goto restart;
    }

    dhcp_offer_dns = 1;
    ret = esp_netif_dhcps_option(wifi_ap_netif,
                                 ESP_NETIF_OP_SET,
                                 ESP_NETIF_DOMAIN_NAME_SERVER,
                                 &dhcp_offer_dns,
                                 sizeof(dhcp_offer_dns));
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "AP DHCP DNS offer update failed: %s", esp_err_to_name(ret));
        goto restart;
    }

    SOLAR_OS_LOGI(TAG, "AP DHCP DNS set to " IPSTR, IP2STR(&dns.ip.u_addr.ip4));

restart:
    if (restart_dhcps) {
        esp_err_t start_ret = esp_netif_dhcps_start(wifi_ap_netif);
        if (start_ret != ESP_OK && start_ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            SOLAR_OS_LOGW(TAG, "AP DHCP restart failed after DNS update: %s", esp_err_to_name(start_ret));
            return ret == ESP_OK ? start_ret : ret;
        }
    }
    return ret;
}

static wifi_mode_t wifi_desired_mode(void)
{
    const bool station_required = wifi_sta_enabled || wifi_connectionless_active;
    if (station_required && wifi_ap_enabled) {
        return WIFI_MODE_APSTA;
    }
    if (wifi_ap_enabled) {
        return WIFI_MODE_AP;
    }
    if (station_required) {
        return WIFI_MODE_STA;
    }
    return WIFI_MODE_NULL;
}

static esp_err_t wifi_apply_mode(void)
{
    const wifi_mode_t mode = wifi_desired_mode();

    if (mode == WIFI_MODE_NULL) {
        (void)wifi_apply_nat();

        if (!wifi_started) {
            return ESP_OK;
        }

        const esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            return stop_err;
        }

        wifi_lock();
        wifi_started = false;
        wifi_ap_running = false;
        wifi_ap_station_count = 0;
        wifi_nat_active = false;
        wifi_set_started_state(false);
        wifi_unlock();
        return ESP_OK;
    }

    esp_err_t ret = esp_wifi_set_mode(mode);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!wifi_started) {
        ret = esp_wifi_start();
        if (ret != ESP_OK) {
            return ret;
        }

    }

    ret = esp_wifi_set_ps(wifi_power_save_mode());
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Wi-Fi power save setup failed: %s", esp_err_to_name(ret));
    }

    wifi_lock();
    wifi_started = true;
    if ((wifi_sta_enabled || wifi_connectionless_active) && !wifi_connected) {
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    } else if (!wifi_sta_enabled && !wifi_connectionless_active) {
        wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    }
    wifi_unlock();
    (void)wifi_apply_nat();
    return ESP_OK;
}

static void wifi_update_saved_config(void)
{
    wifi_lock();
    wifi_refresh_saved_config_locked();
    wifi_unlock();
}

static void wifi_clear_link_state(void)
{
    wifi_connected = false;
    wifi_has_ip = false;
    wifi_rssi = 0;
    wifi_channel = 0;
    wifi_ip[0] = '\0';
    wifi_gateway[0] = '\0';
    wifi_netmask[0] = '\0';
}

static void wifi_refresh_link_info(void)
{
    bool should_refresh = false;

    wifi_lock();
    should_refresh = wifi_initialized && wifi_connected;
    wifi_unlock();

    if (!should_refresh) {
        return;
    }

    wifi_ap_record_t ap_info = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap_info);

    wifi_lock();
    if (!wifi_connected) {
        wifi_unlock();
        return;
    }
    if (err == ESP_OK) {
        wifi_rssi = ap_info.rssi;
        wifi_channel = ap_info.primary;
        if (wifi_connectionless_active) {
            wifi_connectionless_channel = ap_info.primary;
        }
        wifi_copy_ssid(wifi_ssid, sizeof(wifi_ssid), ap_info.ssid, sizeof(ap_info.ssid));
    } else {
        wifi_rssi = 0;
        wifi_channel = 0;
    }
    wifi_unlock();
}

static void wifi_set_started_state(bool started)
{
    wifi_started = started;
    if (!started) {
        wifi_clear_link_state();
        wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    } else if ((wifi_sta_enabled || wifi_connectionless_active) && !wifi_connected) {
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    } else if (!wifi_sta_enabled && !wifi_connectionless_active) {
        wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    }
}

static void wifi_disconnect_for_reconfig(void)
{
    const esp_err_t err = esp_wifi_disconnect();
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            wifi_lock();
            wifi_set_started_state(true);
            wifi_unlock();
            break;
        case WIFI_EVENT_STA_STOP:
            wifi_lock();
            wifi_clear_link_state();
            if (!wifi_suspended) {
                wifi_sta_enabled = false;
            }
            if (!wifi_ap_enabled) {
                wifi_set_started_state(false);
            } else {
                wifi_state = SOLAR_OS_WIFI_STATE_OFF;
            }
            wifi_unlock();
            break;
        case WIFI_EVENT_AP_START:
            wifi_lock();
            wifi_started = true;
            wifi_ap_running = true;
            if (wifi_connectionless_active) {
                wifi_connectionless_channel = wifi_ap_channel;
            }
            wifi_unlock();
            solar_os_wifi_repeater_on_ap_started();
            break;
        case WIFI_EVENT_AP_STOP:
            wifi_lock();
            wifi_ap_running = false;
            wifi_ap_station_count = 0;
            if (!wifi_sta_enabled && !wifi_connectionless_active) {
                wifi_started = false;
            }
            wifi_unlock();
            break;
        case WIFI_EVENT_AP_STACONNECTED:
            wifi_lock();
            if (wifi_ap_station_count < UINT8_MAX) {
                wifi_ap_station_count++;
            }
            wifi_unlock();
            break;
        case WIFI_EVENT_AP_STADISCONNECTED:
            wifi_lock();
            if (wifi_ap_station_count > 0) {
                wifi_ap_station_count--;
            }
            wifi_unlock();
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *event = (const wifi_event_sta_connected_t *)event_data;
            wifi_lock();
            wifi_connected = true;
            wifi_has_ip = false;
            wifi_disconnect_reason = 0;
            wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
            wifi_channel = event != NULL ? event->channel : 0;
            if (wifi_ap_enabled && event != NULL) {
                wifi_ap_channel = event->channel;
            }
            if (wifi_connectionless_active && event != NULL) {
                wifi_connectionless_channel = event->channel;
            }
            if (event != NULL) {
                wifi_copy_ssid(wifi_ssid, sizeof(wifi_ssid), event->ssid, event->ssid_len);
            }
            wifi_unlock();
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *event = (const wifi_event_sta_disconnected_t *)event_data;
            wifi_lock();
            wifi_clear_link_state();
            wifi_disconnect_reason = event != NULL ? event->reason : 0;
            if (event != NULL && event->ssid_len > 0) {
                wifi_copy_ssid(wifi_ssid, sizeof(wifi_ssid), event->ssid, event->ssid_len);
            }
            wifi_state = wifi_started ? SOLAR_OS_WIFI_STATE_DISCONNECTED : SOLAR_OS_WIFI_STATE_OFF;
            wifi_unlock();
            solar_os_wifi_repeater_clear_clients();
            wifi_repeater_schedule_reconnect();
            break;
        }
        default:
            break;
        }
        (void)wifi_apply_nat();
        return;
    }

    if (event_base == IP_EVENT) {
        switch (event_id) {
        case IP_EVENT_STA_GOT_IP: {
            const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
            wifi_ap_record_t ap_info = {0};
            const esp_err_t ap_err = esp_wifi_sta_get_ap_info(&ap_info);

            wifi_lock();
            wifi_has_ip = true;
            wifi_connected = true;
            wifi_state = SOLAR_OS_WIFI_STATE_CONNECTED;
            if (event != NULL) {
                wifi_format_ip(&event->ip_info.ip, wifi_ip, sizeof(wifi_ip));
                wifi_format_ip(&event->ip_info.gw, wifi_gateway, sizeof(wifi_gateway));
                wifi_format_ip(&event->ip_info.netmask, wifi_netmask, sizeof(wifi_netmask));
            }
            if (ap_err == ESP_OK) {
                wifi_rssi = ap_info.rssi;
                wifi_channel = ap_info.primary;
                if (wifi_connectionless_active) {
                    wifi_connectionless_channel = ap_info.primary;
                }
                wifi_copy_ssid(wifi_ssid, sizeof(wifi_ssid), ap_info.ssid, sizeof(ap_info.ssid));
            }
            wifi_unlock();
            wifi_repeater_cancel_reconnect();
            if (event != NULL) {
                solar_os_wifi_repeater_on_upstream_ip(&event->ip_info);
                if (solar_os_wifi_repeater_is_enabled()) {
                    wifi_lock();
                    wifi_format_ip(&event->ip_info.ip, wifi_ap_ip, sizeof(wifi_ap_ip));
                    wifi_unlock();
                }
            }
            break;
        }
        case IP_EVENT_STA_LOST_IP:
            wifi_lock();
            wifi_has_ip = false;
            wifi_ip[0] = '\0';
            wifi_gateway[0] = '\0';
            wifi_netmask[0] = '\0';
            if (wifi_connected) {
                wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
            }
            wifi_unlock();
            solar_os_wifi_repeater_clear_clients();
            break;
        default:
            break;
        }
        (void)wifi_apply_nat();
    }
}

const char *solar_os_wifi_state_name(solar_os_wifi_state_t state)
{
    switch (state) {
    case SOLAR_OS_WIFI_STATE_OFF:
        return "off";
    case SOLAR_OS_WIFI_STATE_IDLE:
        return "idle";
    case SOLAR_OS_WIFI_STATE_SCANNING:
        return "scanning";
    case SOLAR_OS_WIFI_STATE_CONNECTING:
        return "connecting";
    case SOLAR_OS_WIFI_STATE_CONNECTED:
        return "connected";
    case SOLAR_OS_WIFI_STATE_DISCONNECTED:
        return "disconnected";
    case SOLAR_OS_WIFI_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

esp_err_t solar_os_wifi_init(void)
{
    if (wifi_initialized) {
        return ESP_OK;
    }

    if (!solar_os_wifi_enabled_for_current_boot()) {
        return ESP_ERR_NOT_ALLOWED;
    }

    if (wifi_mutex == NULL) {
        wifi_mutex = xSemaphoreCreateMutex();
        if (wifi_mutex == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    wifi_sta_netif = esp_netif_create_default_wifi_sta();
    if (wifi_sta_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_ap_netif = esp_netif_create_default_wifi_ap();
    if (wifi_ap_netif == NULL) {
        return ESP_FAIL;
    }

    char hostname[SOLAR_OS_IDENTITY_HOSTNAME_MAX];
    solar_os_identity_get_hostname(hostname, sizeof(hostname));
    ret = esp_netif_set_hostname(wifi_sta_netif, hostname);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_netif_set_hostname(wifi_ap_netif, hostname);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&config);
    if (ret != ESP_OK) {
        return ret;
    }

    /* SolarOS persists station and AP profiles itself. Keep the driver's
     * working configuration in RAM so esp_wifi_set_config() does not duplicate
     * it in the small shared NVS partition or fail when that partition is full.
     */
    ret = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_set_mode(WIFI_MODE_NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_instance_register(WIFI_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    const esp_timer_create_args_t repeater_timer_args = {
        .callback = wifi_repeater_reconnect_callback,
        .name = "wifi_repeater",
    };
    ret = esp_timer_create(&repeater_timer_args, &wifi_repeater_reconnect_timer);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT,
                                              IP_EVENT_STA_LOST_IP,
                                              wifi_event_handler,
                                              NULL,
                                              NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    wifi_initialized = true;
    wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    wifi_ap_channel = SOLAR_OS_WIFI_DEFAULT_AP_CHANNEL;
    wifi_ap_max_connections = SOLAR_OS_WIFI_DEFAULT_AP_MAX_CONNECTIONS;
    strlcpy(wifi_ap_auth, "open", sizeof(wifi_ap_auth));
    strlcpy(wifi_saved_ap_auth, "open", sizeof(wifi_saved_ap_auth));
    wifi_format_ip(&(esp_ip4_addr_t){0}, wifi_ap_ip, sizeof(wifi_ap_ip));
    wifi_unlock();

    ret = wifi_load_saved_ap_config();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "saved AP config load failed: %s", esp_err_to_name(ret));
    }
    ret = wifi_load_profiles();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "Wi-Fi profile load failed: %s", esp_err_to_name(ret));
    }
    ret = wifi_migrate_legacy_station_config();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "legacy Wi-Fi config migration failed: %s", esp_err_to_name(ret));
    }
    ret = wifi_load_nat_config();
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "NAT config load failed: %s", esp_err_to_name(ret));
    }
    wifi_update_saved_config();
    SOLAR_OS_LOGI(TAG, "Wi-Fi service ready as %s", hostname);
    return ESP_OK;
}

esp_err_t solar_os_wifi_start(void)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    wifi_sta_enabled = true;
    wifi_unlock();

    ret = wifi_apply_mode();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
        return ret;
    }

    wifi_update_saved_config();
    return ESP_OK;
}

esp_err_t solar_os_wifi_stop(void)
{
    wifi_repeater_cancel_reconnect();
    if (!wifi_initialized || !wifi_started) {
        (void)solar_os_wifi_repeater_disable();
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_OFF;
        wifi_unlock();
        return ESP_OK;
    }

    (void)solar_os_wifi_repeater_disable();
    (void)esp_wifi_disconnect();

    wifi_lock();
    wifi_sta_enabled = false;
    wifi_ap_enabled = false;
    wifi_ap_running = false;
    wifi_ap_station_count = 0;
    wifi_unlock();

    return wifi_apply_mode();
}

esp_err_t solar_os_wifi_prepare_sleep(void)
{
    if (!wifi_initialized) {
        return ESP_OK;
    }

    wifi_lock();
    if (wifi_suspended) {
        wifi_unlock();
        return ESP_OK;
    }

    wifi_sleep_was_started = wifi_started;
    wifi_sleep_sta_enabled = wifi_sta_enabled;
    wifi_sleep_ap_enabled = wifi_ap_enabled;
    wifi_sleep_reconnect_sta = wifi_sta_enabled &&
        (wifi_connected ||
         wifi_has_ip ||
         wifi_state == SOLAR_OS_WIFI_STATE_CONNECTING ||
         wifi_state == SOLAR_OS_WIFI_STATE_DISCONNECTED);
    wifi_suspended = true;
    wifi_unlock();
    wifi_repeater_cancel_reconnect();

    if (!wifi_sleep_was_started) {
        return ESP_OK;
    }

    const esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_STARTED) {
        wifi_lock();
        wifi_suspended = false;
        wifi_unlock();
        return ret;
    }

    wifi_lock();
    wifi_set_started_state(false);
    wifi_sta_enabled = wifi_sleep_sta_enabled;
    wifi_ap_enabled = wifi_sleep_ap_enabled;
    wifi_ap_running = false;
    wifi_ap_station_count = 0;
    wifi_unlock();

    SOLAR_OS_LOGI(TAG,
                  "sleep: Wi-Fi radio stopped, sta=%s ap=%s reconnect=%s",
                  wifi_sleep_sta_enabled ? "on" : "off",
                  wifi_sleep_ap_enabled ? "on" : "off",
                  wifi_sleep_reconnect_sta ? "yes" : "no");
    return ESP_OK;
}

esp_err_t solar_os_wifi_resume(void)
{
    wifi_lock();
    if (!wifi_suspended) {
        wifi_unlock();
        return ESP_OK;
    }

    const bool was_started = wifi_sleep_was_started;
    const bool reconnect_sta = wifi_sleep_reconnect_sta;
    wifi_sta_enabled = wifi_sleep_sta_enabled;
    wifi_ap_enabled = wifi_sleep_ap_enabled;
    wifi_suspended = false;
    wifi_unlock();

    if (!was_started) {
        return ESP_OK;
    }

    esp_err_t ret = wifi_apply_mode();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
        return ret;
    }

    wifi_lock();
    const bool restore_connectionless_channel =
        wifi_connectionless_active && !wifi_connected && !wifi_ap_enabled;
    const uint8_t connectionless_channel = wifi_connectionless_channel;
    wifi_unlock();
    if (restore_connectionless_channel && connectionless_channel != 0U) {
        ret = esp_wifi_set_channel(connectionless_channel, WIFI_SECOND_CHAN_NONE);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    if (reconnect_sta) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
        wifi_unlock();

        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            wifi_lock();
            wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
            wifi_unlock();
            return ret;
        }
    }

    SOLAR_OS_LOGI(TAG,
                  "resume: Wi-Fi radio restored, sta=%s ap=%s reconnect=%s",
                  wifi_sta_enabled ? "on" : "off",
                  wifi_ap_enabled ? "on" : "off",
                  reconnect_sta ? "yes" : "no");
    return ESP_OK;
}

esp_netif_t *solar_os_wifi_get_sta_netif(void)
{
    return wifi_sta_netif;
}

esp_err_t solar_os_wifi_connect(const char *ssid, const char *password)
{
    if (wifi_connectionless_fixed()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = wifi_validate_station_settings(ssid, password);
    if (ret != ESP_OK) {
        return ret;
    }
    if (password == NULL) {
        password = "";
    }

    ret = solar_os_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    wifi_sta_enabled = true;
    wifi_unlock();

    wifi_profile_t profile = {0};
    strlcpy(profile.ssid, ssid, sizeof(profile.ssid));
    strlcpy(profile.password, password, sizeof(profile.password));

    wifi_disconnect_for_reconfig();
    ret = wifi_program_station_config(&profile);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
        return ret;
    }

    ret = wifi_upsert_profile(ssid, password);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    strlcpy(wifi_ssid, ssid, sizeof(wifi_ssid));
    wifi_refresh_saved_config_locked();
    wifi_has_ip = false;
    wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
    wifi_unlock();

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
        return ret;
    }

    return ESP_OK;
}

esp_err_t solar_os_wifi_connect_saved(void)
{
    if (wifi_connectionless_fixed()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = solar_os_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_profile_t profile = {0};
    ret = wifi_select_saved_profile(&profile);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_disconnect_for_reconfig();
    ret = wifi_program_station_config(&profile);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
        return ret;
    }

    ret = wifi_upsert_profile(profile.ssid, profile.password);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    strlcpy(wifi_ssid, profile.ssid, sizeof(wifi_ssid));
    wifi_refresh_saved_config_locked();
    wifi_has_ip = false;
    wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
    wifi_unlock();

    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = SOLAR_OS_WIFI_STATE_FAILED;
        wifi_unlock();
    }
    return ret;
}

esp_err_t solar_os_wifi_disconnect(void)
{
    if (!wifi_initialized || !wifi_started) {
        return ESP_OK;
    }
    if (solar_os_wifi_repeater_is_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    const esp_err_t ret = esp_wifi_disconnect();
    if (ret != ESP_OK &&
        ret != ESP_ERR_WIFI_NOT_STARTED &&
        ret != ESP_ERR_WIFI_NOT_CONNECT) {
        return ret;
    }

    wifi_lock();
    wifi_clear_link_state();
    wifi_disconnect_reason = 0;
    wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    wifi_unlock();
    return ESP_OK;
}

esp_err_t solar_os_wifi_forget(void)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    char selected_ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
    wifi_lock();
    if (wifi_ssid[0] != '\0' && wifi_find_profile_index_locked(wifi_ssid) >= 0) {
        strlcpy(selected_ssid, wifi_ssid, sizeof(selected_ssid));
    } else if (wifi_profile_count > 0) {
        strlcpy(selected_ssid, wifi_profiles[0].ssid, sizeof(selected_ssid));
    }
    wifi_unlock();

    if (selected_ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }

    return solar_os_wifi_forget_ssid(selected_ssid);
}

esp_err_t solar_os_wifi_forget_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > SOLAR_OS_WIFI_SSID_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    bool removed_current = false;
    wifi_profile_t next_profile = {0};
    bool has_next_profile = false;

    wifi_lock();
    const int index = wifi_find_profile_index_locked(ssid);
    if (index < 0) {
        wifi_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    removed_current = wifi_ssid[0] != '\0' && strcmp(wifi_ssid, ssid) == 0;
    if ((size_t)index + 1 < wifi_profile_count) {
        memmove(&wifi_profiles[index],
                &wifi_profiles[index + 1],
                (wifi_profile_count - (size_t)index - 1) * sizeof(wifi_profiles[0]));
    }
    wifi_profile_count--;
    memset(&wifi_profiles[wifi_profile_count], 0, sizeof(wifi_profiles[wifi_profile_count]));
    if (wifi_profile_count > 0) {
        next_profile = wifi_profiles[0];
        has_next_profile = true;
    }
    wifi_refresh_saved_config_locked();
    wifi_unlock();

    ret = wifi_save_profiles();
    if (ret != ESP_OK) {
        return ret;
    }

    if (removed_current) {
        (void)esp_wifi_disconnect();
    }

    ret = wifi_program_station_config(has_next_profile ? &next_profile : NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    if (removed_current || !has_next_profile) {
        wifi_lock();
        wifi_clear_link_state();
        if (removed_current || !has_next_profile) {
            wifi_ssid[0] = '\0';
        }
        wifi_disconnect_reason = 0;
        wifi_state = wifi_sta_enabled ? SOLAR_OS_WIFI_STATE_IDLE : SOLAR_OS_WIFI_STATE_OFF;
        wifi_unlock();
    }

    return ESP_OK;
}

esp_err_t solar_os_wifi_forget_all(void)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    (void)esp_wifi_disconnect();

    wifi_lock();
    wifi_clear_profiles_locked();
    wifi_clear_link_state();
    wifi_ssid[0] = '\0';
    wifi_disconnect_reason = 0;
    wifi_state = wifi_sta_enabled ? SOLAR_OS_WIFI_STATE_IDLE : SOLAR_OS_WIFI_STATE_OFF;
    wifi_unlock();

    ret = wifi_save_profiles();
    if (ret != ESP_OK) {
        return ret;
    }

    return wifi_program_station_config(NULL);
}

esp_err_t solar_os_wifi_known(solar_os_wifi_profile_t *profiles, size_t max_profiles, size_t *count)
{
    if (count != NULL) {
        *count = 0;
    }
    if (max_profiles > 0 && profiles == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    const size_t copy_count = wifi_profile_count < max_profiles ? wifi_profile_count : max_profiles;
    for (size_t i = 0; i < copy_count; i++) {
        strlcpy(profiles[i].ssid, wifi_profiles[i].ssid, sizeof(profiles[i].ssid));
        profiles[i].preferred = i == 0;
    }
    if (count != NULL) {
        *count = wifi_profile_count;
    }
    wifi_unlock();
    return ESP_OK;
}

bool solar_os_wifi_is_known_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return false;
    }

    bool known = false;
    wifi_lock();
    known = wifi_find_profile_index_locked(ssid) >= 0;
    wifi_unlock();
    return known;
}

static esp_err_t wifi_ap_start_config(const char *ssid,
                                      const char *password,
                                      const char *auth,
                                      bool persist)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    char selected_ssid[SOLAR_OS_WIFI_SSID_MAX + 1] = {0};
    char selected_password[SOLAR_OS_WIFI_PASSWORD_MAX] = {0};
    char selected_auth[SOLAR_OS_WIFI_AUTH_MAX] = {0};
    const bool use_saved = ssid == NULL || ssid[0] == '\0';

    if (use_saved) {
        bool has_saved = false;
        wifi_lock();
        has_saved = wifi_has_saved_ap_config;
        if (has_saved) {
            strlcpy(selected_ssid, wifi_saved_ap_ssid, sizeof(selected_ssid));
            strlcpy(selected_password, wifi_saved_ap_password, sizeof(selected_password));
            strlcpy(selected_auth, wifi_saved_ap_auth, sizeof(selected_auth));
        }
        wifi_unlock();

        if (!has_saved) {
            strlcpy(selected_ssid, SOLAR_OS_WIFI_DEFAULT_AP_SSID, sizeof(selected_ssid));
            selected_password[0] = '\0';
            strlcpy(selected_auth, "open", sizeof(selected_auth));
        }
    } else {
        wifi_auth_mode_t unused_authmode = WIFI_AUTH_OPEN;
        ret = wifi_validate_ap_settings(ssid, password, auth, &unused_authmode);
        if (ret != ESP_OK) {
            return ret;
        }

        strlcpy(selected_ssid, ssid, sizeof(selected_ssid));
        if (password != NULL) {
            strlcpy(selected_password, password, sizeof(selected_password));
        }
        if (auth != NULL) {
            strlcpy(selected_auth, auth, sizeof(selected_auth));
        }
    }

    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    ret = wifi_validate_ap_settings(selected_ssid, selected_password, selected_auth, &authmode);
    if (ret != ESP_OK) {
        return ret;
    }

    if (!use_saved && persist) {
        ret = wifi_save_ap_config(selected_ssid, selected_password, authmode);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    wifi_config_t config = {0};
    const size_t ssid_len = strlen(selected_ssid);
    const size_t password_len = strlen(selected_password);
    memcpy(config.ap.ssid, selected_ssid, ssid_len);
    config.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(config.ap.password, selected_password, password_len);
    config.ap.channel = wifi_connected && wifi_channel != 0 ?
        wifi_channel :
        SOLAR_OS_WIFI_DEFAULT_AP_CHANNEL;
    config.ap.authmode = authmode;
    config.ap.max_connection = SOLAR_OS_WIFI_DEFAULT_AP_MAX_CONNECTIONS;
    config.ap.beacon_interval = 100;
    config.ap.pmf_cfg.required = false;

    wifi_lock();
    const bool channel_conflict =
        wifi_connectionless_active &&
        !wifi_connectionless_channel_auto &&
        wifi_connectionless_channel != config.ap.channel;
    wifi_unlock();
    if (channel_conflict) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_lock();
    wifi_ap_enabled = true;
    wifi_unlock();

    ret = wifi_apply_mode();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_ap_enabled = false;
        wifi_ap_running = false;
        wifi_unlock();
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_ap_enabled = false;
        wifi_ap_running = false;
        wifi_unlock();
        (void)wifi_apply_mode();
        return ret;
    }

    wifi_lock();
    wifi_copy_ssid(wifi_ap_ssid, sizeof(wifi_ap_ssid), config.ap.ssid, config.ap.ssid_len);
    strlcpy(wifi_ap_auth, wifi_auth_name(authmode), sizeof(wifi_ap_auth));
    wifi_ap_channel = config.ap.channel;
    wifi_ap_max_connections = config.ap.max_connection;
    wifi_unlock();

    if (wifi_ap_netif != NULL) {
        esp_netif_ip_info_t ip_info = {0};
        if (esp_netif_get_ip_info(wifi_ap_netif, &ip_info) == ESP_OK) {
            wifi_lock();
            wifi_format_ip(&ip_info.ip, wifi_ap_ip, sizeof(wifi_ap_ip));
            wifi_unlock();
        }
    }

    return ESP_OK;
}

esp_err_t solar_os_wifi_ap_start(const char *ssid, const char *password, const char *auth)
{
    return wifi_ap_start_config(ssid, password, auth, true);
}

esp_err_t solar_os_wifi_ap_stop(void)
{
    if (!wifi_initialized) {
        return ESP_OK;
    }

    const esp_err_t repeater_ret = solar_os_wifi_repeater_disable();

    wifi_lock();
    wifi_ap_enabled = false;
    wifi_ap_running = false;
    wifi_ap_station_count = 0;
    wifi_ap_ssid[0] = '\0';
    wifi_ap_auth[0] = '\0';
    wifi_ap_ip[0] = '\0';
    wifi_unlock();

    const esp_err_t mode_ret = wifi_apply_mode();
    return repeater_ret != ESP_OK ? repeater_ret : mode_ret;
}

esp_err_t solar_os_wifi_nat_set(bool enabled)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (enabled && solar_os_wifi_repeater_is_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = wifi_save_nat_config(enabled);
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    wifi_nat_enabled = enabled;
    if (!enabled) {
        wifi_nat_last_error = ESP_OK;
    }
    wifi_unlock();

    return wifi_apply_nat();
}

esp_err_t solar_os_wifi_repeater_start(void)
{
    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (solar_os_wifi_repeater_is_enabled()) {
        return ESP_OK;
    }

    bool station_ready = false;
    bool has_saved_station = false;
    wifi_lock();
    station_ready = wifi_connected || wifi_state == SOLAR_OS_WIFI_STATE_CONNECTING;
    has_saved_station = wifi_has_saved_config;
    wifi_unlock();

    if (!station_ready) {
        if (!has_saved_station) {
            return ESP_ERR_NOT_FOUND;
        }
        ret = solar_os_wifi_connect_saved();
        if (ret != ESP_OK) {
            return ret;
        }
    }

    wifi_profile_t repeater_profile = {0};
    wifi_lock();
    int profile_index = wifi_find_profile_index_locked(wifi_ssid);
    if (profile_index < 0 && wifi_profile_count > 0) {
        profile_index = 0;
    }
    if (profile_index >= 0) {
        repeater_profile = wifi_profiles[profile_index];
    }
    wifi_unlock();
    if (repeater_profile.ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    const char *repeater_auth = repeater_profile.password[0] == '\0' ? "open" : "wpa2";

    bool ap_was_enabled = false;
    wifi_lock();
    ap_was_enabled = wifi_ap_enabled;
    wifi_repeater_starting = true;
    wifi_unlock();

    ret = wifi_ap_start_config(repeater_profile.ssid,
                               repeater_profile.password,
                               repeater_auth,
                               false);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_repeater_starting = false;
        wifi_unlock();
        (void)wifi_apply_nat();
        return ret;
    }

    ret = wifi_wait_for_ap_running();
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_repeater_starting = false;
        wifi_unlock();
        if (!ap_was_enabled) {
            (void)solar_os_wifi_ap_stop();
        }
        (void)wifi_apply_nat();
        return ret;
    }

    /* The AP lwIP netif gets its linkoutput callback only after AP startup. */
    ret = solar_os_wifi_repeater_enable(wifi_ap_netif, wifi_sta_netif);
    wifi_lock();
    wifi_repeater_starting = false;
    wifi_unlock();
    if (ret != ESP_OK) {
        if (!ap_was_enabled) {
            (void)solar_os_wifi_ap_stop();
        }
        (void)wifi_apply_nat();
        return ret;
    }

    wifi_lock();
    strlcpy(wifi_ap_ip, wifi_ip, sizeof(wifi_ap_ip));
    wifi_unlock();

    ret = wifi_apply_nat();
    if (ret != ESP_OK) {
        (void)solar_os_wifi_repeater_disable();
        if (!ap_was_enabled) {
            (void)solar_os_wifi_ap_stop();
        }
        (void)wifi_apply_nat();
        return ret;
    }

    ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        SOLAR_OS_LOGW(TAG, "repeater power-save disable failed: %s", esp_err_to_name(ret));
    }
    return ESP_OK;
}

esp_err_t solar_os_wifi_repeater_stop(void)
{
    wifi_repeater_cancel_reconnect();
    const esp_err_t repeater_ret = solar_os_wifi_repeater_disable();
    const esp_err_t ap_ret = solar_os_wifi_ap_stop();
    if (repeater_ret == ESP_OK && ap_ret == ESP_OK && wifi_started) {
        (void)esp_wifi_set_ps(wifi_power_save_mode());
    }
    return repeater_ret != ESP_OK ? repeater_ret : ap_ret;
}

esp_err_t solar_os_wifi_scan(solar_os_wifi_ap_t *aps, size_t max_aps, size_t *found)
{
    if (found != NULL) {
        *found = 0;
    }
    if (max_aps > 0 && aps == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_lock();
    const bool connectionless_active = wifi_connectionless_active;
    wifi_unlock();
    if (connectionless_active) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = solar_os_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    const solar_os_wifi_state_t previous_state = wifi_state;
    wifi_state = SOLAR_OS_WIFI_STATE_SCANNING;
    wifi_unlock();

    ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = previous_state;
        wifi_unlock();
        return ret;
    }

    uint16_t record_count = max_aps > SOLAR_OS_WIFI_SCAN_MAX_RESULTS ?
        SOLAR_OS_WIFI_SCAN_MAX_RESULTS :
        (uint16_t)max_aps;
    wifi_ap_record_t records[SOLAR_OS_WIFI_SCAN_MAX_RESULTS] = {0};
    ret = esp_wifi_scan_get_ap_records(&record_count, records);
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_state = previous_state;
        wifi_unlock();
        return ret;
    }

    for (uint16_t i = 0; i < record_count; i++) {
        wifi_copy_ssid(aps[i].ssid, sizeof(aps[i].ssid), records[i].ssid, sizeof(records[i].ssid));
        aps[i].hidden = aps[i].ssid[0] == '\0';
        if (aps[i].hidden) {
            strlcpy(aps[i].ssid, "<hidden>", sizeof(aps[i].ssid));
        }
        strlcpy(aps[i].auth, wifi_auth_name(records[i].authmode), sizeof(aps[i].auth));
        aps[i].rssi = records[i].rssi;
        aps[i].channel = records[i].primary;
    }
    if (found != NULL) {
        *found = record_count;
    }

    wifi_lock();
    if (!wifi_started) {
        wifi_state = SOLAR_OS_WIFI_STATE_OFF;
    } else if (wifi_connected && wifi_has_ip) {
        wifi_state = SOLAR_OS_WIFI_STATE_CONNECTED;
    } else if (wifi_connected) {
        wifi_state = SOLAR_OS_WIFI_STATE_CONNECTING;
    } else {
        wifi_state = SOLAR_OS_WIFI_STATE_IDLE;
    }
    wifi_unlock();

    return ESP_OK;
}

esp_err_t solar_os_wifi_connectionless_acquire(const char *owner,
                                               uint8_t requested_channel,
                                               uint8_t *actual_channel)
{
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, SOLAR_OS_WIFI_CONNECTIONLESS_OWNER_MAX) >=
            SOLAR_OS_WIFI_CONNECTIONLESS_OWNER_MAX ||
        requested_channel > 13U) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = solar_os_wifi_init();
    if (ret != ESP_OK) {
        return ret;
    }

    wifi_lock();
    if (wifi_connectionless_active) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t active_channel = wifi_connected && wifi_channel != 0U ?
        wifi_channel :
        (wifi_ap_enabled ? wifi_ap_channel : 0U);
    if (requested_channel != 0U && active_channel != 0U &&
        requested_channel != active_channel) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t selected_channel = active_channel != 0U ?
        active_channel :
        (requested_channel != 0U ? requested_channel : SOLAR_OS_WIFI_DEFAULT_AP_CHANNEL);
    wifi_connectionless_active = true;
    wifi_connectionless_channel_auto = requested_channel == 0U;
    wifi_connectionless_channel = selected_channel;
    strlcpy(wifi_connectionless_owner, owner, sizeof(wifi_connectionless_owner));
    wifi_unlock();

    ret = wifi_apply_mode();
    if (ret == ESP_OK && active_channel == 0U) {
        ret = esp_wifi_set_channel(selected_channel, WIFI_SECOND_CHAN_NONE);
    }
    if (ret != ESP_OK) {
        wifi_lock();
        wifi_connectionless_active = false;
        wifi_connectionless_channel_auto = false;
        wifi_connectionless_channel = 0U;
        wifi_connectionless_owner[0] = '\0';
        wifi_unlock();
        (void)wifi_apply_mode();
        return ret;
    }

    uint8_t primary = selected_channel;
    wifi_second_chan_t secondary = WIFI_SECOND_CHAN_NONE;
    if (esp_wifi_get_channel(&primary, &secondary) != ESP_OK) {
        primary = selected_channel;
    }
    wifi_lock();
    wifi_connectionless_channel = primary;
    wifi_unlock();
    if (actual_channel != NULL) {
        *actual_channel = primary;
    }
    SOLAR_OS_LOGI(TAG,
                  "connectionless radio acquired owner=%s channel=%u mode=%s",
                  owner,
                  (unsigned)primary,
                  requested_channel == 0U ? "auto" : "fixed");
    return ESP_OK;
}

esp_err_t solar_os_wifi_connectionless_release(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_lock();
    if (!wifi_connectionless_active) {
        wifi_unlock();
        return ESP_OK;
    }
    if (strcmp(owner, wifi_connectionless_owner) != 0) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    wifi_connectionless_active = false;
    wifi_connectionless_channel_auto = false;
    wifi_connectionless_channel = 0U;
    wifi_connectionless_owner[0] = '\0';
    wifi_unlock();

    const esp_err_t ret = wifi_apply_mode();
    SOLAR_OS_LOGI(TAG, "connectionless radio released owner=%s", owner);
    return ret;
}

esp_err_t solar_os_wifi_latency_acquire(const char *owner)
{
    if (owner == NULL || owner[0] == '\0' ||
        strnlen(owner, SOLAR_OS_WIFI_LATENCY_OWNER_MAX) >=
            SOLAR_OS_WIFI_LATENCY_OWNER_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_lock();
    if (!wifi_initialized || !wifi_started) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (wifi_latency_owner[0] != '\0') {
        const bool already_acquired = strcmp(owner, wifi_latency_owner) == 0;
        wifi_unlock();
        return already_acquired ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    strlcpy(wifi_latency_owner, owner, sizeof(wifi_latency_owner));
    wifi_unlock();

    const esp_err_t ret = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ret != ESP_OK) {
        wifi_lock();
        if (strcmp(owner, wifi_latency_owner) == 0) {
            wifi_latency_owner[0] = '\0';
        }
        const bool started = wifi_started;
        const wifi_ps_type_t restore_mode = wifi_power_save_mode_locked();
        wifi_unlock();
        if (started) {
            (void)esp_wifi_set_ps(restore_mode);
        }
        return ret;
    }

    SOLAR_OS_LOGI(TAG, "low-latency Wi-Fi acquired owner=%s", owner);
    return ESP_OK;
}

esp_err_t solar_os_wifi_latency_release(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_lock();
    if (wifi_latency_owner[0] == '\0') {
        wifi_unlock();
        return ESP_OK;
    }
    if (strcmp(owner, wifi_latency_owner) != 0) {
        wifi_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    wifi_latency_owner[0] = '\0';
    const bool started = wifi_started;
    const wifi_ps_type_t mode = wifi_power_save_mode_locked();
    wifi_unlock();

    const esp_err_t ret = started ? esp_wifi_set_ps(mode) : ESP_OK;
    SOLAR_OS_LOGI(TAG, "low-latency Wi-Fi released owner=%s", owner);
    return ret;
}

void solar_os_wifi_get_status(solar_os_wifi_status_t *status)
{
    if (status == NULL) {
        return;
    }

    wifi_refresh_link_info();

    solar_os_wifi_repeater_status_t repeater_status = {0};
    solar_os_wifi_repeater_get_status(&repeater_status);

    wifi_lock();
    *status = (solar_os_wifi_status_t){
        .state = wifi_state,
        .initialized = wifi_initialized,
        .started = wifi_started,
        .connected = wifi_connected,
        .has_ip = wifi_has_ip,
        .has_saved_config = wifi_has_saved_config,
        .has_saved_ap_config = wifi_has_saved_ap_config,
        .nat_enabled = wifi_nat_enabled,
        .nat_active = wifi_nat_active,
        .repeater_enabled = repeater_status.enabled,
        .repeater_active = repeater_status.enabled && wifi_ap_running &&
            wifi_connected && wifi_has_ip,
        .ap_enabled = wifi_ap_enabled,
        .ap_running = wifi_ap_running,
        .connectionless_active = wifi_connectionless_active,
        .connectionless_channel_auto = wifi_connectionless_channel_auto,
        .rssi = wifi_rssi,
        .channel = wifi_channel,
        .disconnect_reason = wifi_disconnect_reason,
        .ap_channel = wifi_ap_channel,
        .ap_station_count = wifi_ap_station_count,
        .ap_max_connections = wifi_ap_max_connections,
        .connectionless_channel = wifi_connectionless_channel,
        .saved_profile_count = (uint8_t)wifi_profile_count,
        .nat_last_error = wifi_nat_last_error,
        .repeater_learned_clients = (uint8_t)repeater_status.learned_clients,
        .repeater_upstream_frames = repeater_status.upstream_frames,
        .repeater_downstream_frames = repeater_status.downstream_frames,
        .repeater_dropped_frames = repeater_status.dropped_frames,
    };
    strlcpy(status->ssid, wifi_ssid, sizeof(status->ssid));
    strlcpy(status->saved_ssid, wifi_saved_ssid, sizeof(status->saved_ssid));
    strlcpy(status->saved_ap_ssid, wifi_saved_ap_ssid, sizeof(status->saved_ap_ssid));
    strlcpy(status->saved_ap_auth, wifi_saved_ap_auth, sizeof(status->saved_ap_auth));
    strlcpy(status->ip, wifi_ip, sizeof(status->ip));
    strlcpy(status->gateway, wifi_gateway, sizeof(status->gateway));
    strlcpy(status->netmask, wifi_netmask, sizeof(status->netmask));
    strlcpy(status->ap_ssid, wifi_ap_ssid, sizeof(status->ap_ssid));
    strlcpy(status->ap_auth, wifi_ap_auth, sizeof(status->ap_auth));
    strlcpy(status->ap_ip, wifi_ap_ip, sizeof(status->ap_ip));
    strlcpy(status->connectionless_owner,
            wifi_connectionless_owner,
            sizeof(status->connectionless_owner));
    wifi_unlock();
}

void solar_os_wifi_get_status_text(char *buffer, size_t len)
{
    if (buffer == NULL || len == 0) {
        return;
    }

    solar_os_wifi_status_t status;
    solar_os_wifi_get_status(&status);

    if (!status.initialized ||
        (status.state == SOLAR_OS_WIFI_STATE_OFF && !status.ap_running &&
         !status.connectionless_active)) {
        strlcpy(buffer, "off", len);
    } else if (status.state == SOLAR_OS_WIFI_STATE_CONNECTED && status.has_ip) {
        snprintf(buffer,
                 len,
                 "up %s%s%s",
                 status.ip,
                 status.ap_running ? " ap" : "",
                 status.repeater_active ? " repeater" :
                 (status.nat_active ? " nat" : ""));
    } else if (status.ap_running) {
        snprintf(buffer,
                 len,
                 "ap %s%s",
                 status.ap_ip[0] != '\0' ? status.ap_ip : status.ap_ssid,
                 status.repeater_enabled ? " repeater-wait" :
                 (status.nat_active ? " nat" : ""));
    } else if (status.connectionless_active) {
        snprintf(buffer,
                 len,
                 "espnow ch%u",
                 (unsigned)status.connectionless_channel);
    } else if (status.state == SOLAR_OS_WIFI_STATE_CONNECTING && status.ssid[0] != '\0') {
        snprintf(buffer, len, "connecting %s", status.ssid);
    } else if (status.state == SOLAR_OS_WIFI_STATE_DISCONNECTED && status.disconnect_reason != 0) {
        snprintf(buffer, len, "down r%u", (unsigned)status.disconnect_reason);
    } else {
        strlcpy(buffer, solar_os_wifi_state_name(status.state), len);
    }
}
