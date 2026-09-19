#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

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
    esp_err_t last_error;
    char address[19];
    char endpoint[SOLAR_OS_WIREGUARD_ENDPOINT_MAX + 1U];
    char endpoint_ip[16];
    char dns[16];
    char peer_key_fingerprint[17];
    char underlay[SOLAR_OS_WIREGUARD_UNDERLAY_MAX + 1U];
} solar_os_wireguard_status_t;

esp_err_t solar_os_wireguard_init(void);
esp_err_t solar_os_wireguard_import(const char *path, char *detail, size_t detail_len);
esp_err_t solar_os_wireguard_forget(void);
esp_err_t solar_os_wireguard_up(solar_os_wireguard_policy_t policy);
esp_err_t solar_os_wireguard_down(void);
esp_err_t solar_os_wireguard_prepare_sleep(void);
esp_err_t solar_os_wireguard_resume(void);
void solar_os_wireguard_get_status(solar_os_wireguard_status_t *status);
const char *solar_os_wireguard_state_name(solar_os_wireguard_state_t state);
const char *solar_os_wireguard_policy_name(solar_os_wireguard_policy_t policy);

/* Platform callbacks required by the pinned wireguard-lwIP component. */
uint32_t wireguard_sys_now(void);
void wireguard_random_bytes(void *bytes, size_t size);
void wireguard_tai64n_now(uint8_t *output);
bool wireguard_is_under_load(void);
