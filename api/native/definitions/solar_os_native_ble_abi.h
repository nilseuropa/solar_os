#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_BLE_CLIENT_SERVICE "ble.client"
#define SOLAR_OS_NATIVE_BLE_CLIENT_ABI 1U
#define SOLAR_OS_NATIVE_BLE_NAME_MAX 64U
#define SOLAR_OS_NATIVE_BLE_UUID_MAX 37U
#define SOLAR_OS_NATIVE_BLE_VALUE_MAX 176U
#define SOLAR_OS_NATIVE_BLE_SESSION_INVALID 0U
#define SOLAR_OS_NATIVE_BLE_PEER_INVALID 0U

typedef uint32_t solar_os_native_ble_session_t;
typedef uint32_t solar_os_native_ble_peer_t;

typedef enum {
    SOLAR_OS_NATIVE_BLE_OK = 0,
    SOLAR_OS_NATIVE_BLE_ERROR_FAILED = -1,
    SOLAR_OS_NATIVE_BLE_ERROR_INVALID_ARGUMENT = -2,
    SOLAR_OS_NATIVE_BLE_ERROR_INVALID_STATE = -3,
    SOLAR_OS_NATIVE_BLE_ERROR_NO_MEMORY = -4,
    SOLAR_OS_NATIVE_BLE_ERROR_NOT_FOUND = -5,
    SOLAR_OS_NATIVE_BLE_ERROR_NOT_SUPPORTED = -6,
    SOLAR_OS_NATIVE_BLE_ERROR_TIMEOUT = -7,
    SOLAR_OS_NATIVE_BLE_ERROR_CANCELLED = -8,
    SOLAR_OS_NATIVE_BLE_ERROR_CAPACITY = -9,
    SOLAR_OS_NATIVE_BLE_ERROR_INVALID_SIZE = -10,
} solar_os_native_ble_result_v1_t;

typedef enum {
    SOLAR_OS_NATIVE_BLE_ADDR_PUBLIC = 0,
    SOLAR_OS_NATIVE_BLE_ADDR_RANDOM = 1,
    SOLAR_OS_NATIVE_BLE_ADDR_PUBLIC_IDENTITY = 2,
    SOLAR_OS_NATIVE_BLE_ADDR_RANDOM_IDENTITY = 3,
} solar_os_native_ble_addr_type_v1_t;

typedef enum {
    SOLAR_OS_NATIVE_BLE_CHAR_BROADCAST = 0x01,
    SOLAR_OS_NATIVE_BLE_CHAR_READ = 0x02,
    SOLAR_OS_NATIVE_BLE_CHAR_WRITE_NO_RESPONSE = 0x04,
    SOLAR_OS_NATIVE_BLE_CHAR_WRITE = 0x08,
    SOLAR_OS_NATIVE_BLE_CHAR_NOTIFY = 0x10,
    SOLAR_OS_NATIVE_BLE_CHAR_INDICATE = 0x20,
    SOLAR_OS_NATIVE_BLE_CHAR_SIGNED_WRITE = 0x40,
    SOLAR_OS_NATIVE_BLE_CHAR_EXTENDED = 0x80,
} solar_os_native_ble_characteristic_property_v1_t;

typedef enum {
    SOLAR_OS_NATIVE_BLE_UNSUBSCRIBE = 0,
    SOLAR_OS_NATIVE_BLE_SUBSCRIBE_NOTIFY = 1,
    SOLAR_OS_NATIVE_BLE_SUBSCRIBE_INDICATE = 2,
} solar_os_native_ble_subscription_mode_v1_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    bool hid_service;
    bool keyboard_like;
    bool remembered;
    bool connected;
    char name[SOLAR_OS_NATIVE_BLE_NAME_MAX];
} solar_os_native_ble_scan_result_v1_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    bool primary;
    char uuid[SOLAR_OS_NATIVE_BLE_UUID_MAX];
} solar_os_native_ble_service_v1_t;

typedef struct {
    uint16_t handle;
    uint8_t properties;
    char uuid[SOLAR_OS_NATIVE_BLE_UUID_MAX];
} solar_os_native_ble_characteristic_v1_t;

typedef struct {
    bool connected;
    bool encrypted;
    bool bonded;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t conn_id;
    uint16_t mtu;
    size_t service_count;
    char status[80];
} solar_os_native_ble_gatt_status_v1_t;

typedef struct {
    char owner[32];
    bool busy;
    bool retiring;
    size_t event_capacity;
    size_t event_count;
    uint32_t events_dropped;
    solar_os_native_ble_gatt_status_v1_t gatt;
} solar_os_native_ble_peer_info_v1_t;

typedef struct {
    uint16_t handle;
    bool indication;
    size_t value_len;
    uint8_t value[SOLAR_OS_NATIVE_BLE_VALUE_MAX];
} solar_os_native_ble_notification_v1_t;

/* All blocking operations must run outside a native job tick callback. The
 * host owns connections and notification storage; modules retain only opaque
 * session and peer handles and must close the session during stop. */
typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t max_value_size;
    int (*init)(void);
    int (*scan)(solar_os_native_ble_scan_result_v1_t *results,
                size_t max_results, size_t *found);
    int (*session_create)(const char *owner,
                          solar_os_native_ble_session_t *session);
    int (*session_cancel)(solar_os_native_ble_session_t session);
    int (*session_close)(solar_os_native_ble_session_t session);
    size_t (*peer_capacity)(void);
    int (*peer_connect)(solar_os_native_ble_session_t session,
                        const uint8_t bda[6], uint8_t addr_type,
                        uint32_t timeout_ms, solar_os_native_ble_peer_t *peer);
    int (*peer_disconnect)(solar_os_native_ble_session_t session,
                           solar_os_native_ble_peer_t peer);
    int (*peer_pair)(solar_os_native_ble_session_t session,
                     solar_os_native_ble_peer_t peer, uint32_t passkey,
                     uint32_t timeout_ms);
    int (*peer_get_info)(solar_os_native_ble_session_t session,
                         solar_os_native_ble_peer_t peer,
                         solar_os_native_ble_peer_info_v1_t *info);
    int (*peer_services)(solar_os_native_ble_session_t session,
                         solar_os_native_ble_peer_t peer,
                         solar_os_native_ble_service_v1_t *services,
                         size_t max_services, size_t *count);
    int (*peer_characteristics)(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                size_t service_index,
                                solar_os_native_ble_characteristic_v1_t *chars,
                                size_t max_chars, size_t *count);
    int (*peer_configure_queue)(solar_os_native_ble_session_t session,
                                solar_os_native_ble_peer_t peer,
                                size_t capacity);
    int (*peer_subscribe)(solar_os_native_ble_session_t session,
                          solar_os_native_ble_peer_t peer, uint16_t handle,
                          uint8_t mode, uint32_t timeout_ms);
    int (*peer_poll)(solar_os_native_ble_session_t session,
                     solar_os_native_ble_peer_t peer,
                     solar_os_native_ble_notification_v1_t *event);
    int (*peer_read)(solar_os_native_ble_session_t session,
                     solar_os_native_ble_peer_t peer, uint16_t handle,
                     uint8_t *value, size_t max_len, size_t *value_len,
                     uint32_t timeout_ms);
    int (*peer_write)(solar_os_native_ble_session_t session,
                      solar_os_native_ble_peer_t peer, uint16_t handle,
                      const uint8_t *value, size_t value_len,
                      bool with_response, uint32_t timeout_ms);
} solar_os_native_ble_client_api_v1_t;

#ifdef __cplusplus
}
#endif
