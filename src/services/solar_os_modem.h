#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "solar_os_native_modem_abi.h"

typedef solar_os_native_modem_profile_v1_t solar_os_modem_profile_t;
typedef solar_os_native_modem_status_v1_t solar_os_modem_status_t;
typedef solar_os_native_modem_transport_rate_info_v1_t solar_os_modem_transport_rate_info_t;
typedef solar_os_native_modem_info_v1_t solar_os_modem_info_t;

typedef struct {
    esp_err_t (*get_status)(void *ctx, solar_os_modem_status_t *status);
    esp_err_t (*set_power)(void *ctx, bool enabled);
    esp_err_t (*reset)(void *ctx);
    esp_err_t (*apply_profile)(void *ctx,
                               const solar_os_modem_profile_t *profile);
    esp_err_t (*clear_profile)(void *ctx);
    esp_err_t (*set_data_active)(void *ctx, bool active);
    esp_err_t (*get_transport_rate)(
        void *ctx,
        solar_os_modem_transport_rate_info_t *info);
    esp_err_t (*set_transport_rate)(void *ctx, uint32_t rate);
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
    bool powered;
} solar_os_modem_registration_t;

esp_err_t solar_os_modem_register(
    const solar_os_modem_registration_t *registration);
esp_err_t solar_os_modem_unregister(const char *name);
size_t solar_os_modem_count(void);
bool solar_os_modem_get(size_t index, solar_os_modem_info_t *info);

esp_err_t solar_os_modem_get_status(const char *name,
                                    solar_os_modem_status_t *status);
esp_err_t solar_os_modem_set_power(const char *name, bool enabled);
esp_err_t solar_os_modem_reset(const char *name);
esp_err_t solar_os_modem_profile_set(const char *name,
                                     const solar_os_modem_profile_t *profile);
esp_err_t solar_os_modem_profile_get(const char *name,
                                     solar_os_modem_profile_t *profile);
esp_err_t solar_os_modem_profile_clear(const char *name);
esp_err_t solar_os_modem_set_data_active(const char *name, bool active);
esp_err_t solar_os_modem_transport_rate_get(
    const char *name,
    solar_os_modem_transport_rate_info_t *info);
esp_err_t solar_os_modem_transport_rate_set(const char *name, uint32_t rate);
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
const char *solar_os_modem_network_state_name(
    solar_os_modem_network_state_t state);
