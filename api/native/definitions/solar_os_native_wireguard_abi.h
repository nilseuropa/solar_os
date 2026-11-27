#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_WIREGUARD_SERVICE "network.wireguard"
#define SOLAR_OS_NATIVE_WIREGUARD_ABI 1U

#define SOLAR_OS_WIREGUARD_ENDPOINT_MAX 127U
#define SOLAR_OS_WIREGUARD_ROUTE_MAX 8U
#define SOLAR_OS_WIREGUARD_UNDERLAY_MAX 20U

typedef enum {
    SOLAR_OS_WIREGUARD_STATE_OFF = 0,
    SOLAR_OS_WIREGUARD_STATE_WAIT_UPLINK,
    SOLAR_OS_WIREGUARD_STATE_RESOLVING,
    SOLAR_OS_WIREGUARD_STATE_CONNECTING,
    SOLAR_OS_WIREGUARD_STATE_UP,
    SOLAR_OS_WIREGUARD_STATE_SUSPENDED,
    SOLAR_OS_WIREGUARD_STATE_ERROR,
} solar_os_wireguard_state_t;

typedef enum {
    SOLAR_OS_WIREGUARD_POLICY_AUTO = 0,
    SOLAR_OS_WIREGUARD_POLICY_FAIL_OPEN,
    SOLAR_OS_WIREGUARD_POLICY_FAIL_CLOSED,
} solar_os_wireguard_policy_t;

typedef struct {
    solar_os_wireguard_state_t state;
    solar_os_wireguard_policy_t policy;
    bool initialized;
    bool configured;
    bool desired_up;
    bool peer_up;
    bool full_tunnel;
    bool routes_active;
    bool default_route_active;
    bool kill_switch_active;
    bool dns_configured;
    uint8_t route_count;
    uint16_t endpoint_port;
    uint16_t listen_port;
    uint16_t keepalive_seconds;
    uint16_t mtu;
    int32_t last_error;
    char address[19];
    char endpoint[SOLAR_OS_WIREGUARD_ENDPOINT_MAX + 1U];
    char endpoint_ip[16];
    char dns[16];
    char peer_key_fingerprint[17];
    char underlay[SOLAR_OS_WIREGUARD_UNDERLAY_MAX + 1U];
} solar_os_wireguard_status_t;

/* Operate the shared WireGuard tunnel. Import reads a resolved storage path
 * and copies the profile into Core configuration. Status contains the public
 * peer fingerprint and route state. The private key remains in Core. Names
 * returned by state_name and policy_name have static lifetime. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*import_profile)(const char *path, char *detail, size_t detail_capacity);
    int (*forget)(void);
    int (*up)(solar_os_wireguard_policy_t policy);
    int (*down)(void);
    int (*status)(solar_os_wireguard_status_t *status);
    const char *(*state_name)(solar_os_wireguard_state_t state);
    const char *(*policy_name)(solar_os_wireguard_policy_t policy);
} solar_os_native_wireguard_api_v1_t;
