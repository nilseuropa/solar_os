#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Host-independent BLE API. Numeric addresses/properties use Bluetooth wire
 * values; no Bluedroid or NimBLE headers are part of this contract. */
#define SOLAR_OS_BLE_NAME_MAX 64
#define SOLAR_OS_BLE_SCAN_MAX_RESULTS 32
#define SOLAR_OS_BLE_GATT_UUID_MAX 37
#define SOLAR_OS_BLE_GATT_MAX_SERVICES 24
#define SOLAR_OS_BLE_GATT_MAX_CHARACTERISTICS 64
#define SOLAR_OS_BLE_GATT_VALUE_MAX 128
#define SOLAR_OS_BLE_CONNECTION_INVALID UINT16_MAX
#define SOLAR_OS_BLE_OWNER_MAX 32
#define SOLAR_OS_BLE_SESSION_INVALID 0U
#define SOLAR_OS_BLE_ERR_CANCELLED ((esp_err_t)0xB1E0)
#define SOLAR_OS_BLE_ERR_CAPACITY ((esp_err_t)0xB1E1)
#define SOLAR_OS_BLE_PEER_INVALID 0U

typedef uint32_t solar_os_ble_session_t;
typedef uint32_t solar_os_ble_peer_t;

/* Native HID-over-GATT peripheral. The OS owns the standard report map,
 * descriptors, encryption, bonding and teardown; scripts only submit typed
 * keyboard, mouse and gamepad state. One runtime owns the connectable
 * peripheral lease, shared with the generic application server. */
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_NUM_LOCK    0x01U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK   0x02U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_SCROLL_LOCK 0x04U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_COMPOSE     0x08U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_KANA        0x10U

typedef enum {
    SOLAR_OS_BLE_HID_CONNECTED,
    SOLAR_OS_BLE_HID_DISCONNECTED,
    SOLAR_OS_BLE_HID_SECURED,
    SOLAR_OS_BLE_HID_KEYBOARD_LEDS,
} solar_os_ble_hid_device_event_type_t;

typedef struct {
    solar_os_ble_hid_device_event_type_t type;
    uint32_t peer;
    uint16_t status;
    uint8_t keyboard_leds;
    bool encrypted;
    bool bonded;
} solar_os_ble_hid_device_event_t;

typedef struct {
    bool registered;
    bool advertising;
    bool closing;
    bool connected;
    bool encrypted;
    bool bonded;
    bool keyboard_subscribed;
    bool mouse_subscribed;
    bool gamepad_subscribed;
    uint8_t keyboard_leds;
    size_t event_capacity;
    size_t event_count;
    uint32_t events_dropped;
} solar_os_ble_hid_info_t;

esp_err_t solar_os_ble_hid_device_start(solar_os_ble_session_t session,
                                        const char *name);
esp_err_t solar_os_ble_hid_device_stop(solar_os_ble_session_t session);
esp_err_t solar_os_ble_hid_device_status(solar_os_ble_session_t session,
                                         solar_os_ble_hid_info_t *info);
esp_err_t solar_os_ble_hid_device_poll(solar_os_ble_session_t session,
                                       solar_os_ble_hid_device_event_t *event);
esp_err_t solar_os_ble_hid_keyboard_press(solar_os_ble_session_t session,
                                          const uint16_t *keys,
                                          size_t key_count);
esp_err_t solar_os_ble_hid_keyboard_release(solar_os_ble_session_t session,
                                            const uint16_t *keys,
                                            size_t key_count);
esp_err_t solar_os_ble_hid_keyboard_release_all(solar_os_ble_session_t session);
esp_err_t solar_os_ble_hid_mouse_move(solar_os_ble_session_t session,
                                      int32_t x,
                                      int32_t y);
esp_err_t solar_os_ble_hid_mouse_button(solar_os_ble_session_t session,
                                        uint8_t button,
                                        bool pressed);
esp_err_t solar_os_ble_hid_gamepad_axis(solar_os_ble_session_t session,
                                        int axis,
                                        int16_t value);
esp_err_t solar_os_ble_hid_gamepad_button(solar_os_ble_session_t session,
                                          uint8_t button,
                                          bool pressed);
esp_err_t solar_os_ble_hid_gamepad_hat(solar_os_ble_session_t session,
                                       uint8_t hat);
esp_err_t solar_os_ble_hid_gamepad_send(solar_os_ble_session_t session);

/* Application peripheral API. One owned legacy-advertising lease; incoming
 * links share the stack/controller capacity with outgoing peers. Local IDs and
 * peer IDs are opaque, not ATT handles. Reads use stored values, never callbacks.
 * Requests copy all data; events contain no backend/interpreter pointers. */
typedef enum {
    SOLAR_OS_BLE_SERVER_CREATE, SOLAR_OS_BLE_SERVER_SERVICE,
    SOLAR_OS_BLE_SERVER_CHARACTERISTIC, SOLAR_OS_BLE_SERVER_START,
    SOLAR_OS_BLE_SERVER_STOP, SOLAR_OS_BLE_SERVER_CLOSE,
    SOLAR_OS_BLE_SERVER_STATUS, SOLAR_OS_BLE_SERVER_POLL,
    SOLAR_OS_BLE_SERVER_SET, SOLAR_OS_BLE_SERVER_SEND,
    SOLAR_OS_BLE_SERVER_DISCONNECT, SOLAR_OS_BLE_SERVER_PEER,
} solar_os_ble_server_operation_t;
typedef enum {
    SOLAR_OS_BLE_SERVER_CONNECTED, SOLAR_OS_BLE_SERVER_DISCONNECTED,
    SOLAR_OS_BLE_SERVER_READ, SOLAR_OS_BLE_SERVER_WRITE,
    SOLAR_OS_BLE_SERVER_SUBSCRIBE, SOLAR_OS_BLE_SERVER_SENT,
} solar_os_ble_server_event_type_t;
typedef struct {
    solar_os_ble_server_event_type_t type;
    uint32_t peer, characteristic;
    uint16_t mtu, status;
    bool notify, indicate;
    size_t value_len;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
} solar_os_ble_server_event_t;
typedef struct {
    bool registered, advertising, closing;
    size_t peers, services, characteristics, event_capacity, event_count;
    uint32_t events_dropped;
} solar_os_ble_server_info_t;
typedef struct {
    solar_os_ble_server_operation_t op;
    uint32_t id, parent, peer;
    uint8_t properties;
    bool indicate;
    char text[SOLAR_OS_BLE_GATT_UUID_MAX]; /* UUID, or CREATE name (1..26 bytes). */
    size_t capacity, value_len;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
    solar_os_ble_server_info_t info;
    solar_os_ble_server_event_t event;
} solar_os_ble_server_request_t;
esp_err_t solar_os_ble_server_request(solar_os_ble_session_t session,
                                     solar_os_ble_server_request_t *request);
typedef bool (*solar_os_ble_cancel_check_t)(void *user);

typedef enum {
    SOLAR_OS_BLE_ADDR_PUBLIC = 0,
    SOLAR_OS_BLE_ADDR_RANDOM = 1,
    SOLAR_OS_BLE_ADDR_PUBLIC_IDENTITY = 2,
    SOLAR_OS_BLE_ADDR_RANDOM_IDENTITY = 3,
} solar_os_ble_addr_type_t;

typedef enum {
    SOLAR_OS_BLE_CHAR_BROADCAST = 0x01,
    SOLAR_OS_BLE_CHAR_READ = 0x02,
    SOLAR_OS_BLE_CHAR_WRITE_NO_RESPONSE = 0x04,
    SOLAR_OS_BLE_CHAR_WRITE = 0x08,
    SOLAR_OS_BLE_CHAR_NOTIFY = 0x10,
    SOLAR_OS_BLE_CHAR_INDICATE = 0x20,
    SOLAR_OS_BLE_CHAR_SIGNED_WRITE = 0x40,
    SOLAR_OS_BLE_CHAR_EXTENDED = 0x80,
} solar_os_ble_characteristic_property_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    int8_t rssi;
    uint16_t appearance;
    bool hid_service;
    bool keyboard_like;
    bool remembered;
    bool connected;
    char name[SOLAR_OS_BLE_NAME_MAX];
} solar_os_ble_scan_result_t;

typedef struct {
    bool connected;
    uint8_t bda[6];
    uint8_t addr_type;
    uint16_t conn_id;
    uint16_t mtu;
    size_t service_count;
    char status[80];
} solar_os_ble_gatt_status_t;

typedef struct {
    uint16_t start_handle;
    uint16_t end_handle;
    bool primary;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_service_t;

typedef struct {
    uint16_t handle;
    uint8_t properties;
    char uuid[SOLAR_OS_BLE_GATT_UUID_MAX];
} solar_os_ble_gatt_characteristic_t;

typedef struct {
    char owner[SOLAR_OS_BLE_OWNER_MAX];
    bool busy;
    bool retiring;
    size_t event_capacity, event_count;
    uint32_t events_dropped;
    solar_os_ble_gatt_status_t gatt;
} solar_os_ble_session_info_t;

typedef struct {
    uint16_t handle;
    bool indication;
    size_t value_len;
    uint8_t value[SOLAR_OS_BLE_GATT_VALUE_MAX];
} solar_os_ble_notification_t;

/* Owners retain a session handle and close it on exit (including errors).
 * Handles are generation checked; owner names are diagnostic, not credentials.
 * Sessions and peers are allocated dynamically. Each peer permits one blocking
 * operation; cancel/close may run from another task. Session close/cancel affects
 * all its peers. The session_* data operations retain a legacy default peer.
 * Cancellation aborts the connection and wakes a waiter with CANCELLED; it
 * cannot undo a write already sent. Close invalidates the handle immediately.
 * Transport reuse waits for backend retirement, even after close has returned.
 * All calls except the internal event sink run outside the Bluetooth task. */
esp_err_t solar_os_ble_session_create(const char *owner, solar_os_ble_session_t *session);
/* Optional cooperative cancellation, checked by the waiting caller every 50 ms.
 * The check must return normally (no VM exceptions), must not block, and its
 * context must outlive the operation. Change it only while the session is idle.
 * NULL disables the check. No callback runs on the Bluetooth task. */
esp_err_t solar_os_ble_session_set_cancel_check(solar_os_ble_session_t session,
    solar_os_ble_cancel_check_t check, void *user);
/* Strict colon-separated hexadecimal address, in display order. */
bool solar_os_ble_parse_address(const char *text, size_t len, uint8_t bda[6]);
esp_err_t solar_os_ble_session_close(solar_os_ble_session_t session);
esp_err_t solar_os_ble_session_cancel(solar_os_ble_session_t session);
esp_err_t solar_os_ble_session_get_info(solar_os_ble_session_t session,
                                      solar_os_ble_session_info_t *info);
esp_err_t solar_os_ble_session_connect(solar_os_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t solar_os_ble_session_services(solar_os_ble_session_t session,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count);
esp_err_t solar_os_ble_session_characteristics(solar_os_ble_session_t session,
    size_t service_index, solar_os_ble_gatt_characteristic_t *characteristics,
    size_t max_characteristics, size_t *count);
esp_err_t solar_os_ble_session_read(solar_os_ble_session_t session,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms);
esp_err_t solar_os_ble_session_write(solar_os_ble_session_t session,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms);

/* Explicit peer handles are scoped to their owning session, never transport IDs.
 * Connect allocates a new peer and returns its handle only on success. Disconnect
 * invalidates that handle; storage survives outstanding callers and retirement.
 * Capacity is the configured host/controller budget minus reserved HID capacity.
 * CAPACITY and NO_MEM leave existing connections intact. Connect attempts can
 * return INVALID_STATE while another GAP connection establishment is in progress.
 * Peer handles survive remote disconnect/sleep for status; disconnect releases
 * them, then connect returns a fresh handle with fresh discovery. */
size_t solar_os_ble_peer_capacity(void);
/* Queue storage is allocated on demand (default 16 on first subscribe).
 * Configure only on an idle connected peer with an empty queue. Allocation
 * failure preserves the old queue. Full queues drop NEW events; oversized
 * values are dropped whole, never truncated. Status exposes a saturating loss
 * counter. Poll is nonblocking and returns NOT_FOUND when empty.
 * Disconnect/cancel/sleep discard queued events and release queue storage. */
esp_err_t solar_os_ble_peer_configure_queue(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, size_t capacity);
esp_err_t solar_os_ble_peer_poll(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, solar_os_ble_notification_t *event);
/* mode: 0 unsubscribe, 1 notifications, 2 indications. Discovers the CCCD
 * within this characteristic's descriptor range and waits for its write ACK.
 * Timeout/cancellation retires only this peer. No app callback runs on the host.
 * Poll may run during a pending subscription. Successful unsubscribe discards
 * queued events for its handle; other subscriptions/queues are unaffected. */
esp_err_t solar_os_ble_peer_subscribe(solar_os_ble_session_t session,
    solar_os_ble_peer_t peer, uint16_t handle, uint8_t mode, uint32_t timeout_ms);
esp_err_t solar_os_ble_peer_connect(solar_os_ble_session_t session,
    const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms, solar_os_ble_peer_t *peer);
esp_err_t solar_os_ble_peer_disconnect(solar_os_ble_session_t session, solar_os_ble_peer_t peer);
esp_err_t solar_os_ble_peer_get_info(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    solar_os_ble_session_info_t *info);
esp_err_t solar_os_ble_peer_services(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    solar_os_ble_gatt_service_t *services, size_t max_services, size_t *count);
esp_err_t solar_os_ble_peer_characteristics(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    size_t service_index, solar_os_ble_gatt_characteristic_t *chars, size_t max_chars, size_t *count);
esp_err_t solar_os_ble_peer_read(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    uint16_t handle, uint8_t *value, size_t max_len, size_t *value_len, uint32_t timeout_ms);
esp_err_t solar_os_ble_peer_write(solar_os_ble_session_t session, solar_os_ble_peer_t peer,
    uint16_t handle, const uint8_t *value, size_t value_len, bool with_response, uint32_t timeout_ms);

/* Lifecycle calls retain the current boot policy and the OS keyboard profile.
 * Scan results include keyboard hints for compatibility with the shell.
 * Lifecycle transitions and backend submissions are serialized by the service.
 * Sleep cancels generic operations; session handles survive sleep until closed. */
esp_err_t solar_os_ble_init(void);
esp_err_t solar_os_ble_scan(solar_os_ble_scan_result_t *results, size_t max_results, size_t *found);
esp_err_t solar_os_ble_prepare_sleep(uint32_t timeout_ms);
bool solar_os_ble_sleep_prepare_ready(void);
void solar_os_ble_resume(void);

/* Compatibility client: reserved shell session, one operation at a time.
 * conn_id is a diagnostic transport identifier, not an app-owned handle.
 * UUIDs are formatted as 0xNNNN, 0xNNNNNNNN, or canonical 128-bit strings.
 * These synchronous calls must run outside the Bluetooth callback task.
 * A zero timeout selects the existing service default. Timeouts retire the
 * connection. Writes without response wait for local completion, not peer ACK.
 * Reconnect may return INVALID_STATE until backend retirement completes. */
esp_err_t solar_os_ble_gatt_connect(const uint8_t bda[6], uint8_t addr_type, uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_disconnect(void);
void solar_os_ble_gatt_get_status(solar_os_ble_gatt_status_t *status);
esp_err_t solar_os_ble_gatt_services(solar_os_ble_gatt_service_t *services,
                                     size_t max_services,
                                     size_t *count);
esp_err_t solar_os_ble_gatt_characteristics(size_t service_index,
                                            solar_os_ble_gatt_characteristic_t *characteristics,
                                            size_t max_characteristics,
                                            size_t *count);
esp_err_t solar_os_ble_gatt_read(uint16_t handle,
                                 uint8_t *value,
                                 size_t max_len,
                                 size_t *value_len,
                                 uint32_t timeout_ms);
esp_err_t solar_os_ble_gatt_write(uint16_t handle,
                                  const uint8_t *value,
                                  size_t value_len,
                                  bool with_response,
                                  uint32_t timeout_ms);
