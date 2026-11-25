#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_WIFI_SERVICE "wifi"
#define SOLAR_OS_NATIVE_WIFI_ABI 1U
#define SOLAR_OS_NATIVE_WIFI_OFF 0U
#define SOLAR_OS_NATIVE_WIFI_IDLE 1U
#define SOLAR_OS_NATIVE_WIFI_SCANNING 2U
#define SOLAR_OS_NATIVE_WIFI_CONNECTING 3U
#define SOLAR_OS_NATIVE_WIFI_CONNECTED 4U
#define SOLAR_OS_NATIVE_WIFI_DISCONNECTED 5U
#define SOLAR_OS_NATIVE_WIFI_FAILED 6U
#define SOLAR_OS_NATIVE_WIFI_INITIALIZED (1U << 0)
#define SOLAR_OS_NATIVE_WIFI_STARTED (1U << 1)
#define SOLAR_OS_NATIVE_WIFI_LINK (1U << 2)
#define SOLAR_OS_NATIVE_WIFI_HAS_IP (1U << 3)
#define SOLAR_OS_NATIVE_WIFI_SAVED (1U << 4)
#define SOLAR_OS_NATIVE_WIFI_AP_SAVED (1U << 5)
#define SOLAR_OS_NATIVE_WIFI_NAT_ENABLED (1U << 6)
#define SOLAR_OS_NATIVE_WIFI_NAT_ACTIVE (1U << 7)
#define SOLAR_OS_NATIVE_WIFI_REPEATER_ENABLED (1U << 8)
#define SOLAR_OS_NATIVE_WIFI_REPEATER_ACTIVE (1U << 9)
#define SOLAR_OS_NATIVE_WIFI_AP_ENABLED (1U << 10)
#define SOLAR_OS_NATIVE_WIFI_AP_RUNNING (1U << 11)
#define SOLAR_OS_NATIVE_WIFI_CONNECTIONLESS (1U << 12)
#define SOLAR_OS_NATIVE_WIFI_AUTO_CHANNEL (1U << 13)
typedef struct { char ssid[33], auth[18]; int32_t rssi; uint32_t channel, hidden; } solar_os_native_wifi_ap_v1_t;
typedef struct { char ssid[33]; uint32_t preferred; } solar_os_native_wifi_profile_v1_t;
typedef struct { char ssid[33], password[64], auth[18]; } solar_os_native_wifi_ap_config_v1_t;
typedef struct {
    uint64_t repeater_upstream_frames, repeater_downstream_frames, repeater_dropped_frames;
    uint32_t state, flags, channel, disconnect_reason, ap_channel, ap_station_count, ap_max_connections;
    uint32_t connectionless_channel, saved_profile_count, repeater_learned_clients;
    int32_t rssi, nat_error;
    char ssid[33], saved_ssid[33], saved_ap_ssid[33], saved_ap_auth[18];
    char ip[16], gateway[16], netmask[16], ap_ssid[33], ap_auth[18], ap_ip[16], connectionless_owner[32];
} solar_os_native_wifi_status_v1_t;
/* Scan/profile/status records are copied. Radio leases copy owner strings and
 * must be closed explicitly. The OS consumes ownership before applying radio
 * policy; a close error preserves the token only for remaining cleanup.
 * Retry close to finish cleanup without releasing another radio owner.
 * OS owner names include a unique lease prefix and up to 22 label characters.
 * channel 0 requests automatic selection. Boot flags take effect next boot. */
typedef struct {
    uint32_t abi_version, struct_size;
    bool (*boot_enabled)(bool next_boot);
    int (*boot_enable)(bool enabled);
    int (*start)(void);
    int (*stop)(void);
    int (*connect)(const char *ssid, const char *password);
    int (*connect_saved)(void);
    int (*disconnect)(void);
    int (*forget)(const char *ssid); /* NULL forgets the current saved config. */
    int (*forget_all)(void);
    int (*known)(solar_os_native_wifi_profile_v1_t *profiles, size_t capacity, size_t *count);
    bool (*is_known)(const char *ssid);
    int (*ap_start)(const char *ssid, const char *password, const char *auth);
    int (*ap_stop)(void);
    int (*ap_saved)(solar_os_native_wifi_ap_config_v1_t *config);
    int (*ap_save)(const char *ssid, const char *password, const char *auth);
    int (*ap_forget)(void);
    int (*nat_set)(bool enabled);
    int (*repeater_start)(void);
    int (*repeater_stop)(void);
    int (*scan)(solar_os_native_wifi_ap_v1_t *aps, size_t capacity, size_t *count);
    int (*scan_start)(void);
    int (*scan_results)(solar_os_native_wifi_ap_v1_t *aps, size_t capacity, size_t *count);
    int (*scan_cancel)(void);
    int (*connectionless_acquire)(const char *owner, uint32_t channel, uint32_t *actual_channel, solar_os_native_handle_t *lease);
    int (*latency_acquire)(const char *owner, solar_os_native_handle_t *lease);
    int (*lease_close)(solar_os_native_handle_t lease);
    int (*status)(solar_os_native_wifi_status_v1_t *status);
    int (*status_text)(char *text, size_t capacity);
} solar_os_native_wifi_api_v1_t;
