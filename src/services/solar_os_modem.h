#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_MODEM_NAME_MAX 20U
#define SOLAR_OS_MODEM_DRIVER_MAX 24U
#define SOLAR_OS_MODEM_TRANSPORT_MAX 24U
#define SOLAR_OS_MODEM_APN_MAX 100U
#define SOLAR_OS_MODEM_USERNAME_MAX 64U
#define SOLAR_OS_MODEM_PASSWORD_MAX 64U

typedef enum {
    SOLAR_OS_MODEM_IP_IPV4 = 0,
    SOLAR_OS_MODEM_IP_IPV6,
    SOLAR_OS_MODEM_IP_IPV4V6,
} solar_os_modem_ip_type_t;

typedef enum {
    SOLAR_OS_MODEM_AUTH_NONE = 0,
    SOLAR_OS_MODEM_AUTH_PAP,
    SOLAR_OS_MODEM_AUTH_CHAP,
    SOLAR_OS_MODEM_AUTH_AUTO,
} solar_os_modem_auth_t;

typedef enum {
    SOLAR_OS_MODEM_REGISTRATION_UNKNOWN = 0,
    SOLAR_OS_MODEM_REGISTRATION_NOT_REGISTERED,
    SOLAR_OS_MODEM_REGISTRATION_SEARCHING,
    SOLAR_OS_MODEM_REGISTRATION_DENIED,
    SOLAR_OS_MODEM_REGISTRATION_HOME,
    SOLAR_OS_MODEM_REGISTRATION_ROAMING,
} solar_os_modem_network_registration_t;

typedef struct {
    char apn[SOLAR_OS_MODEM_APN_MAX + 1U];
    solar_os_modem_ip_type_t ip_type;
    solar_os_modem_auth_t auth;
    char username[SOLAR_OS_MODEM_USERNAME_MAX + 1U];
    char password[SOLAR_OS_MODEM_PASSWORD_MAX + 1U];
} solar_os_modem_profile_t;

typedef struct {
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
} solar_os_modem_status_t;

typedef struct {
    esp_err_t (*get_status)(void *ctx, solar_os_modem_status_t *status);
    esp_err_t (*apply_profile)(void *ctx,
                               const solar_os_modem_profile_t *profile);
    esp_err_t (*clear_profile)(void *ctx);
    esp_err_t (*set_data_active)(void *ctx, bool active);
    esp_err_t (*unlock_sim)(void *ctx, const char *pin);
    esp_err_t (*command)(void *ctx,
                         const char *command,
                         uint32_t timeout_ms,
                         char *response,
                         size_t response_size);
} solar_os_modem_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const char *transport;
    const solar_os_modem_ops_t *ops;
    void *ctx;
} solar_os_modem_registration_t;

typedef struct {
    char name[SOLAR_OS_MODEM_NAME_MAX];
    char driver[SOLAR_OS_MODEM_DRIVER_MAX];
    char transport[SOLAR_OS_MODEM_TRANSPORT_MAX];
    bool profile_support;
    bool data_control;
    bool sim_unlock;
    bool raw_command;
} solar_os_modem_info_t;

esp_err_t solar_os_modem_register(
    const solar_os_modem_registration_t *registration);
esp_err_t solar_os_modem_unregister(const char *name);
size_t solar_os_modem_count(void);
bool solar_os_modem_get(size_t index, solar_os_modem_info_t *info);

esp_err_t solar_os_modem_get_status(const char *name,
                                    solar_os_modem_status_t *status);
esp_err_t solar_os_modem_profile_set(const char *name,
                                     const solar_os_modem_profile_t *profile);
esp_err_t solar_os_modem_profile_get(const char *name,
                                     solar_os_modem_profile_t *profile);
esp_err_t solar_os_modem_profile_clear(const char *name);
esp_err_t solar_os_modem_set_data_active(const char *name, bool active);
esp_err_t solar_os_modem_unlock_sim(const char *name, const char *pin);
esp_err_t solar_os_modem_command(const char *name,
                                 const char *command,
                                 uint32_t timeout_ms,
                                 char *response,
                                 size_t response_size);

esp_err_t solar_os_modem_profile_validate(
    const solar_os_modem_profile_t *profile);
bool solar_os_modem_ip_type_parse(const char *name,
                                  solar_os_modem_ip_type_t *type);
const char *solar_os_modem_ip_type_name(solar_os_modem_ip_type_t type);
bool solar_os_modem_auth_parse(const char *name, solar_os_modem_auth_t *auth);
const char *solar_os_modem_auth_name(solar_os_modem_auth_t auth);
const char *solar_os_modem_registration_name(
    solar_os_modem_network_registration_t registration);
