#pragma once
#include "solar_os_native_ble_abi.h"

/* Canonical Bluetooth value records shared by core services and the SDK.
 * Client records alias the released ble.client v1 definitions. No host stack
 * types, build settings or internal session objects are exposed. */
#define SOLAR_OS_BLE_NAME_MAX SOLAR_OS_NATIVE_BLE_NAME_MAX
#define SOLAR_OS_BLE_GATT_UUID_MAX SOLAR_OS_NATIVE_BLE_UUID_MAX
#define SOLAR_OS_BLE_GATT_VALUE_MAX SOLAR_OS_NATIVE_BLE_VALUE_MAX
#define SOLAR_OS_BLE_CONNECTION_INVALID UINT16_MAX
#define SOLAR_OS_BLE_SESSION_INVALID 0U
#define SOLAR_OS_BLE_PEER_INVALID 0U

typedef solar_os_native_ble_session_t solar_os_ble_session_t;

typedef solar_os_native_ble_peer_t solar_os_ble_peer_t;

typedef struct {
    uint8_t bda[6];
    uint8_t addr_type;
    bool connected;
    /* Best-effort GAP Device Name of the current managed connection. RAM only;
     * empty while unavailable. Apps own persistent friendly-name caches. */
    char name[SOLAR_OS_BLE_NAME_MAX];
} solar_os_ble_hid_host_t;

typedef enum {
    SOLAR_OS_BLE_HID_CONNECTED,
    SOLAR_OS_BLE_HID_DISCONNECTED,
    SOLAR_OS_BLE_HID_SECURED,
    SOLAR_OS_BLE_HID_KEYBOARD_LEDS,
    SOLAR_OS_BLE_HID_PASSKEY,
} solar_os_ble_hid_device_event_type_t;

typedef struct {
    solar_os_ble_hid_device_event_type_t type;
    uint32_t peer;
    uint32_t passkey;
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
    bool manual;
    bool pairing;
    bool host_selected;
    uint8_t host_bda[6];
    uint8_t host_addr_type;
    char host_name[SOLAR_OS_BLE_NAME_MAX];
} solar_os_ble_hid_info_t;

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

typedef solar_os_native_ble_scan_result_v1_t solar_os_ble_scan_result_t;

typedef solar_os_native_ble_gatt_status_v1_t solar_os_ble_gatt_status_t;

typedef solar_os_native_ble_service_v1_t solar_os_ble_gatt_service_t;

typedef solar_os_native_ble_characteristic_v1_t solar_os_ble_gatt_characteristic_t;

typedef solar_os_native_ble_peer_info_v1_t solar_os_ble_session_info_t;

typedef solar_os_native_ble_notification_v1_t solar_os_ble_notification_t;
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_NUM_LOCK    0x01U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_CAPS_LOCK   0x02U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_SCROLL_LOCK 0x04U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_COMPOSE     0x08U
#define SOLAR_OS_BLE_HID_KEYBOARD_LED_KANA        0x10U

/* Operation callbacks run before submission and while waiting, synchronously
 * in the waiting/requesting task, outside
 * the BLE metadata lock. Keep code and user data valid until the call returns.
 * Return normally; do not block, throw VM exceptions or reenter BLE operations.
 * The service copies no callback pointers into persistent session state.
 * NULL options or a NULL predicate disable cooperative checking for this call.
 * Explicit session cancel/close still interrupts waiting operations. */
typedef struct {
    uint32_t struct_size;
    solar_os_ble_cancel_check_t cancelled;
    void *user;
} solar_os_ble_call_options_v1_t;

#define SOLAR_OS_BLE_HID_HOST_MAX 8U

typedef enum {
    SOLAR_OS_BLE_HID_OP_START,
    SOLAR_OS_BLE_HID_OP_STOP,
    SOLAR_OS_BLE_HID_OP_STATUS,
    SOLAR_OS_BLE_HID_OP_POLL,
    SOLAR_OS_BLE_HID_OP_KEYBOARD_PRESS,
    SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE,
    SOLAR_OS_BLE_HID_OP_KEYBOARD_RELEASE_ALL,
    SOLAR_OS_BLE_HID_OP_MOUSE_MOVE,
    SOLAR_OS_BLE_HID_OP_MOUSE_BUTTON,
    SOLAR_OS_BLE_HID_OP_GAMEPAD_AXIS,
    SOLAR_OS_BLE_HID_OP_GAMEPAD_BUTTON,
    SOLAR_OS_BLE_HID_OP_GAMEPAD_HAT,
    SOLAR_OS_BLE_HID_OP_GAMEPAD_SEND,
    SOLAR_OS_BLE_HID_OP_PAIR,
    SOLAR_OS_BLE_HID_OP_HOSTS,
    SOLAR_OS_BLE_HID_OP_CONNECT,
    SOLAR_OS_BLE_HID_OP_FORGET,
    SOLAR_OS_BLE_HID_OP_DISCONNECT,
} solar_os_ble_hid_operation_t;

typedef struct {
    solar_os_ble_hid_operation_t op;
    char name[27];
    uint16_t keys[8];
    size_t key_count;
    int32_t x;
    int32_t y;
    int16_t value;
    int axis;
    uint8_t button;
    uint8_t hat;
    bool pressed;
    bool manual;
    uint8_t bda[6], addr_type;
    size_t host_count;
    solar_os_ble_hid_host_t hosts[SOLAR_OS_BLE_HID_HOST_MAX];
    solar_os_ble_hid_info_t info;
    solar_os_ble_hid_device_event_t event;
} solar_os_ble_hid_request_t;
