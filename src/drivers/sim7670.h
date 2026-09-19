#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SIM7670_COMMAND_MAX 160U
#define SIM7670_APN_MAX 100U
#define SIM7670_USERNAME_MAX 64U
#define SIM7670_PASSWORD_MAX 64U

typedef esp_err_t (*sim7670_write_fn_t)(void *user,
                                       const uint8_t *data,
                                       size_t len,
                                       size_t *written);
typedef esp_err_t (*sim7670_read_fn_t)(void *user,
                                      uint8_t *data,
                                      size_t len,
                                      uint32_t timeout_ms,
                                      size_t *read_len);

typedef struct {
    sim7670_write_fn_t write;
    sim7670_read_fn_t read;
    void *user;
} sim7670_io_t;

typedef struct {
    sim7670_io_t io;
    bool initialized;
} sim7670_t;

typedef enum {
    SIM7670_REGISTRATION_UNKNOWN,
    SIM7670_REGISTRATION_NOT_REGISTERED,
    SIM7670_REGISTRATION_SEARCHING,
    SIM7670_REGISTRATION_DENIED,
    SIM7670_REGISTRATION_HOME,
    SIM7670_REGISTRATION_ROAMING,
} sim7670_registration_t;

typedef struct {
    bool online;
    bool sim_status_valid;
    bool sim_ready;
    bool registration_status_valid;
    sim7670_registration_t registration;
    bool signal_status_valid;
    bool rssi_valid;
    int16_t rssi_dbm;
    uint8_t bit_error_rate;
    bool data_status_valid;
    bool data_active;
} sim7670_status_t;

typedef enum {
    SIM7670_PDP_IPV4 = 0,
    SIM7670_PDP_IPV6,
    SIM7670_PDP_IPV4V6,
} sim7670_pdp_type_t;

typedef enum {
    SIM7670_AUTH_NONE = 0,
    SIM7670_AUTH_PAP,
    SIM7670_AUTH_CHAP,
    SIM7670_AUTH_AUTO,
} sim7670_auth_t;

typedef struct {
    bool valid;
    bool time_valid;
    bool satellites_valid;
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t fix_type;
    uint8_t satellites;
    int32_t latitude_deg_e7;
    int32_t longitude_deg_e7;
    int32_t height_msl_mm;
    int32_t ground_speed_mm_s;
    int32_t heading_deg_e5;
    uint16_t position_dop_e2;
} sim7670_gnss_fix_t;

esp_err_t sim7670_init(sim7670_t *device, const sim7670_io_t *io);
esp_err_t sim7670_command(sim7670_t *device,
                          const char *command,
                          uint32_t timeout_ms,
                          char *response,
                          size_t response_size);
esp_err_t sim7670_read_status(sim7670_t *device,
                              sim7670_status_t *status);
esp_err_t sim7670_configure_pdp(sim7670_t *device,
                                const char *apn,
                                sim7670_pdp_type_t pdp_type,
                                sim7670_auth_t auth,
                                const char *username,
                                const char *password);
esp_err_t sim7670_clear_pdp(sim7670_t *device);
esp_err_t sim7670_set_packet_attached(sim7670_t *device, bool attached);
esp_err_t sim7670_set_pdp_active(sim7670_t *device, bool active);
esp_err_t sim7670_enter_data_mode(sim7670_t *device);
esp_err_t sim7670_unlock_sim(sim7670_t *device, const char *pin);
esp_err_t sim7670_set_gnss_power(sim7670_t *device, bool enabled);
esp_err_t sim7670_read_gnss_fix(sim7670_t *device,
                                uint32_t timeout_ms,
                                sim7670_gnss_fix_t *fix);

bool sim7670_parse_csq(const char *response,
                       bool *rssi_valid,
                       int16_t *rssi_dbm,
                       uint8_t *bit_error_rate);
bool sim7670_parse_cereg(const char *response,
                         sim7670_registration_t *registration);
bool sim7670_parse_cgact(const char *response,
                         unsigned context_id,
                         bool *active);
bool sim7670_parse_cgpsinfo(const char *response,
                            sim7670_gnss_fix_t *fix);
bool sim7670_parse_cgnssinfo(const char *response,
                             sim7670_gnss_fix_t *fix);
const char *sim7670_registration_name(sim7670_registration_t registration);
