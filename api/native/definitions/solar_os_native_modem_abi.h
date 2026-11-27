#pragma once

#include "solar_os_native_service_abi.h"

#define SOLAR_OS_NATIVE_MODEM_SERVICE "modem"
#define SOLAR_OS_NATIVE_MODEM_ABI 1U
#define SOLAR_OS_MODEM_SUPPLY 1U
#define SOLAR_OS_MODEM_PACKET_DATA 2U
#define SOLAR_OS_MODEM_LABEL_IP_TYPE 1U
#define SOLAR_OS_MODEM_LABEL_AUTH 2U
#define SOLAR_OS_MODEM_LABEL_REGISTRATION 3U
#define SOLAR_OS_MODEM_LABEL_NETWORK_STATE 4U

#define SOLAR_OS_MODEM_NAME_MAX 20U
#define SOLAR_OS_MODEM_DRIVER_MAX 24U
#define SOLAR_OS_MODEM_TRANSPORT_MAX 24U
#define SOLAR_OS_MODEM_APN_MAX 100U
#define SOLAR_OS_MODEM_DNS_MAX 15U
#define SOLAR_OS_MODEM_USERNAME_MAX 64U
#define SOLAR_OS_MODEM_PASSWORD_MAX 64U
#define SOLAR_OS_MODEM_ADDRESS_MAX 46U
#define SOLAR_OS_MODEM_TRANSPORT_RATE_MAX 12U

typedef uint32_t solar_os_modem_ip_type_t;
#define SOLAR_OS_MODEM_IP_IPV4 0U
#define SOLAR_OS_MODEM_IP_IPV6 1U
#define SOLAR_OS_MODEM_IP_IPV4V6 2U

typedef uint32_t solar_os_modem_auth_t;
#define SOLAR_OS_MODEM_AUTH_NONE 0U
#define SOLAR_OS_MODEM_AUTH_PAP 1U
#define SOLAR_OS_MODEM_AUTH_CHAP 2U
#define SOLAR_OS_MODEM_AUTH_AUTO 3U

typedef uint32_t solar_os_modem_network_registration_t;
#define SOLAR_OS_MODEM_REGISTRATION_UNKNOWN 0U
#define SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED 1U
#define SOLAR_OS_MODEM_REGISTRATION_SEARCHING 2U
#define SOLAR_OS_MODEM_REGISTRATION_DENIED 3U
#define SOLAR_OS_MODEM_REGISTRATION_HOME 4U
#define SOLAR_OS_MODEM_REGISTRATION_ROAMING 5U

typedef uint32_t solar_os_modem_network_state_t;
#define SOLAR_OS_MODEM_NETWORK_UNKNOWN 0U
#define SOLAR_OS_MODEM_NETWORK_DOWN 1U
#define SOLAR_OS_MODEM_NETWORK_CONNECTING 2U
#define SOLAR_OS_MODEM_NETWORK_UP 3U
#define SOLAR_OS_MODEM_NETWORK_FAILED 4U

typedef struct {
    char apn[SOLAR_OS_MODEM_APN_MAX + 1U];
    char dns[SOLAR_OS_MODEM_DNS_MAX + 1U];
    solar_os_modem_ip_type_t ip_type;
    solar_os_modem_auth_t auth;
    char username[SOLAR_OS_MODEM_USERNAME_MAX + 1U];
    char password[SOLAR_OS_MODEM_PASSWORD_MAX + 1U];
} solar_os_native_modem_profile_v1_t;

typedef struct {
    bool power_control;
    bool powered;
    bool online;
    bool sim_status_valid;
    bool sim_ready;
    bool registration_status_valid;
    solar_os_modem_network_registration_t registration;
    bool signal_status_valid;
    bool rssi_valid;
    int16_t rssi_dbm;
    uint8_t bit_error_rate;
    bool data_status_valid;
    bool data_active;
    bool network_status_valid;
    solar_os_modem_network_state_t network_state;
    char network_interface[8];
    char ipv4_address[SOLAR_OS_MODEM_ADDRESS_MAX];
    char ipv4_gateway[SOLAR_OS_MODEM_ADDRESS_MAX];
    char dns_address[SOLAR_OS_MODEM_ADDRESS_MAX];
} solar_os_native_modem_status_v1_t;

typedef struct {
    bool automatic;
    uint32_t configured_rate;
    uint32_t active_rate;
    uint32_t supported_rates[SOLAR_OS_MODEM_TRANSPORT_RATE_MAX];
    size_t supported_rate_count;
} solar_os_native_modem_transport_rate_info_v1_t;

typedef struct {
    char name[SOLAR_OS_MODEM_NAME_MAX];
    char driver[SOLAR_OS_MODEM_DRIVER_MAX];
    char transport[SOLAR_OS_MODEM_TRANSPORT_MAX];
    bool power_control;
    bool powered;
    bool reset_control;
    bool profile_support;
    bool data_control;
    bool transport_rate_control;
    bool sim_unlock;
    bool raw_command;
} solar_os_native_modem_info_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    size_t (*count)(void);
    int (*info)(size_t index, solar_os_native_modem_info_v1_t *info);
    int (*status)(const char *name, solar_os_native_modem_status_v1_t *status);
    /* Supply control and packet-data activation are independent. */
    int (*set_active)(const char *name, uint32_t target, bool active);
    int (*reset)(const char *name);
    int (*profile_read)(const char *name, solar_os_native_modem_profile_v1_t *profile);
    /* NULL removes the configured profile. */
    int (*profile_write)(const char *name, const solar_os_native_modem_profile_v1_t *profile);
    int (*profile_validate)(const solar_os_native_modem_profile_v1_t *profile);
    int (*transport_rate_read)(const char *name, solar_os_native_modem_transport_rate_info_v1_t *info);
    int (*transport_rate_write)(const char *name, uint32_t rate);
    int (*unlock_sim)(const char *name, const char *pin);
    int (*command)(const char *name, const char *command, uint32_t timeout_ms, char *response, size_t capacity);
    int (*parse)(uint32_t kind, const char *text, uint32_t *value);
    const char *(*label)(uint32_t kind, uint32_t value);
} solar_os_native_modem_api_v1_t;
