#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_native_radio_abi.h"
#define SOLAR_OS_RADIO_BUILTIN_PROFILE_COUNT 5
#define SOLAR_OS_RADIO_USER_PROFILE_MAX 8
#define SOLAR_OS_RADIO_PROFILE_MAX \
    (SOLAR_OS_RADIO_BUILTIN_PROFILE_COUNT + SOLAR_OS_RADIO_USER_PROFILE_MAX)

const char *solar_os_radio_rx_gain_name(solar_os_radio_rx_gain_t gain);
bool solar_os_radio_rx_gain_parse(const char *name, uint8_t *gain);
void solar_os_radio_rx_gains_format(uint32_t modes, char *buffer, size_t length);

/* What a driver sets: the config's choice, or its own when there is none. */
static inline bool solar_os_radio_low_data_rate(
    const solar_os_radio_config_t *config, bool automatic)
{
    switch (config->low_data_rate) {
    case SOLAR_OS_RADIO_LOW_DATA_RATE_OFF:
        return false;
    case SOLAR_OS_RADIO_LOW_DATA_RATE_ON:
        return true;
    default:
        return automatic;
    }
}

typedef struct {
    esp_err_t (*configure)(void *ctx, const solar_os_radio_config_t *config);
    esp_err_t (*set_state)(void *ctx, solar_os_radio_state_t state);
    esp_err_t (*get_status)(void *ctx, solar_os_radio_status_t *status);
    esp_err_t (*send)(void *ctx, const solar_os_radio_packet_t *packet, uint32_t timeout_ms);
    esp_err_t (*send_stream)(void *ctx,
                             const uint8_t *data,
                             size_t len,
                             uint32_t timeout_ms);
    esp_err_t (*receive)(void *ctx, solar_os_radio_packet_t *packet, uint32_t timeout_ms);
    /* Read receiver energy without consuming a packet or changing radio state.
     * Return INVALID_STATE outside RX; never return a cached packet RSSI. */
    esp_err_t (*read_rssi)(void *ctx, int16_t *dbm);
    /* Retune an initialized receiver without changing modulation/filter/gain.
     * Leave it in RX and return the minimum RSSI settling interval in ms.
     * Calibration boundaries and timing belong to the driver. */
    esp_err_t (*tune_rx)(void *ctx, uint32_t frequency_hz, uint32_t *settle_ms);
} solar_os_radio_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const char *summary;
    solar_os_radio_modulations_t modulations;
    solar_os_radio_features_t features;
    uint32_t rx_gain_modes;
    size_t max_packet_len;
    solar_os_radio_config_t default_config;
    solar_os_radio_state_t initial_state;
    const solar_os_radio_ops_t *ops;
    void *ctx;
} solar_os_radio_registration_t;

typedef struct {
    int index;
    uint32_t token;
} solar_os_radio_handle_t;

#define SOLAR_OS_RADIO_HANDLE_INIT { .index = -1, .token = 0 }

esp_err_t solar_os_radio_init(void);
esp_err_t solar_os_radio_register(const solar_os_radio_registration_t *registration);
esp_err_t solar_os_radio_unregister(const char *name);
esp_err_t solar_os_radio_unregister_owned(const char *name, const void *ctx);

size_t solar_os_radio_count(void);
bool solar_os_radio_get(size_t index, solar_os_radio_info_t *info);
esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *info);
esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *status);
/* Read-only observation is allowed while another service owns the radio. */
esp_err_t solar_os_radio_read_rssi(const char *name, int16_t *dbm);
esp_err_t solar_os_radio_handle_tune_rx(const solar_os_radio_handle_t *handle,
                                       uint32_t frequency_hz, uint32_t *settle_ms);

esp_err_t solar_os_radio_claim(const char *name,
                               const char *owner,
                               solar_os_radio_handle_t *handle);
esp_err_t solar_os_radio_release(solar_os_radio_handle_t *handle);
bool solar_os_radio_handle_valid(const solar_os_radio_handle_t *handle);
esp_err_t solar_os_radio_handle_get_status(const solar_os_radio_handle_t *handle,
                                           solar_os_radio_status_t *status);
esp_err_t solar_os_radio_handle_configure(const solar_os_radio_handle_t *handle,
                                          const solar_os_radio_config_t *config);
esp_err_t solar_os_radio_handle_set_state(const solar_os_radio_handle_t *handle,
                                          solar_os_radio_state_t state);
esp_err_t solar_os_radio_handle_send(const solar_os_radio_handle_t *handle,
                                     const solar_os_radio_packet_t *packet,
                                     uint32_t timeout_ms);
esp_err_t solar_os_radio_handle_send_stream(const solar_os_radio_handle_t *handle,
                                            const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t solar_os_radio_handle_receive(const solar_os_radio_handle_t *handle,
                                        solar_os_radio_packet_t *packet,
                                        uint32_t timeout_ms);

esp_err_t solar_os_radio_configure(const char *name, const solar_os_radio_config_t *config);
esp_err_t solar_os_radio_set_state(const char *name, solar_os_radio_state_t state);
esp_err_t solar_os_radio_send(const char *name,
                              const solar_os_radio_packet_t *packet,
                              uint32_t timeout_ms);
esp_err_t solar_os_radio_send_stream(const char *name,
                                     const uint8_t *data,
                                     size_t len,
                                     uint32_t timeout_ms);
esp_err_t solar_os_radio_receive(const char *name,
                                 solar_os_radio_packet_t *packet,
                                 uint32_t timeout_ms);

esp_err_t solar_os_radio_profile_list(solar_os_radio_profile_t *profiles,
                                      size_t capacity,
                                      size_t *count);
esp_err_t solar_os_radio_profile_get(const char *profile_name,
                                     solar_os_radio_profile_t *profile);
esp_err_t solar_os_radio_profile_apply(const char *radio_name,
                                       const char *profile_name);
esp_err_t solar_os_radio_handle_profile_apply(
    const solar_os_radio_handle_t *handle,
    const char *profile_name);
esp_err_t solar_os_radio_profile_save(const char *radio_name,
                                      const char *profile_name);
esp_err_t solar_os_radio_profile_remove(const char *profile_name);

const char *solar_os_radio_modulation_name(solar_os_radio_modulation_t modulation);
solar_os_radio_modulation_t solar_os_radio_modulation_from_name(const char *name);
const char *solar_os_radio_feature_name(solar_os_radio_feature_t feature);
const char *solar_os_radio_state_name(solar_os_radio_state_t state);
solar_os_radio_state_t solar_os_radio_state_from_name(const char *name);
void solar_os_radio_modulations_format(solar_os_radio_modulations_t modulations,
                                       char *buffer,
                                       size_t buffer_len);
void solar_os_radio_features_format(solar_os_radio_features_t features,
                                    char *buffer,
                                    size_t buffer_len);
