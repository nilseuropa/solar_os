#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_RADIO_NAME_MAX 20
#define SOLAR_OS_RADIO_DRIVER_MAX 20
#define SOLAR_OS_RADIO_SUMMARY_MAX 48
#define SOLAR_OS_RADIO_SYNC_WORD_MAX 8
#define SOLAR_OS_RADIO_PACKET_MAX 256
#define SOLAR_OS_RADIO_PROFILE_NAME_MAX 24
#define SOLAR_OS_RADIO_OWNER_MAX 24
#define SOLAR_OS_RADIO_USER_PROFILE_MAX 8
#define SOLAR_OS_RADIO_BUILTIN_PROFILE_COUNT 5
#define SOLAR_OS_RADIO_PROFILE_MAX \
    (SOLAR_OS_RADIO_BUILTIN_PROFILE_COUNT + SOLAR_OS_RADIO_USER_PROFILE_MAX)

typedef uint32_t solar_os_radio_modulations_t;
typedef uint32_t solar_os_radio_features_t;

typedef enum {
    SOLAR_OS_RADIO_MODULATION_NONE = 0,
    SOLAR_OS_RADIO_MODULATION_FSK = 1U << 0,
    SOLAR_OS_RADIO_MODULATION_GFSK = 1U << 1,
    SOLAR_OS_RADIO_MODULATION_MSK = 1U << 2,
    SOLAR_OS_RADIO_MODULATION_GMSK = 1U << 3,
    SOLAR_OS_RADIO_MODULATION_OOK = 1U << 4,
    SOLAR_OS_RADIO_MODULATION_LORA = 1U << 5,
} solar_os_radio_modulation_t;

typedef enum {
    SOLAR_OS_RADIO_FEATURE_PACKET = 1U << 0,
    SOLAR_OS_RADIO_FEATURE_RSSI = 1U << 1,
    SOLAR_OS_RADIO_FEATURE_SNR = 1U << 2,
    SOLAR_OS_RADIO_FEATURE_TX_POWER = 1U << 3,
    SOLAR_OS_RADIO_FEATURE_CRC = 1U << 4,
    SOLAR_OS_RADIO_FEATURE_SYNC_WORD = 1U << 5,
    SOLAR_OS_RADIO_FEATURE_PREAMBLE = 1U << 6,
    SOLAR_OS_RADIO_FEATURE_VARIABLE_LENGTH = 1U << 7,
    SOLAR_OS_RADIO_FEATURE_ADDRESSING = 1U << 8,
    SOLAR_OS_RADIO_FEATURE_AES = 1U << 9,
    SOLAR_OS_RADIO_FEATURE_PROMISCUOUS = 1U << 10,
    SOLAR_OS_RADIO_FEATURE_CONTINUOUS_RX = 1U << 11,
    SOLAR_OS_RADIO_FEATURE_CONTINUOUS_TX = 1U << 12,
    SOLAR_OS_RADIO_FEATURE_RX_GAIN = 1U << 13,
    SOLAR_OS_RADIO_FEATURE_LIVE_RSSI = 1U << 14,
    SOLAR_OS_RADIO_FEATURE_FAST_TUNE = 1U << 15,
} solar_os_radio_feature_t;

typedef enum {
    SOLAR_OS_RADIO_RX_GAIN_DEFAULT = 0,
    SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING,
    SOLAR_OS_RADIO_RX_GAIN_BOOSTED,
    SOLAR_OS_RADIO_RX_GAIN_AUTO,
    SOLAR_OS_RADIO_RX_GAIN_G1,
    SOLAR_OS_RADIO_RX_GAIN_G2,
    SOLAR_OS_RADIO_RX_GAIN_G3,
    SOLAR_OS_RADIO_RX_GAIN_G4,
    SOLAR_OS_RADIO_RX_GAIN_G5,
    SOLAR_OS_RADIO_RX_GAIN_G6,
    SOLAR_OS_RADIO_RX_GAIN_COUNT,
} solar_os_radio_rx_gain_t;

#define SOLAR_OS_RADIO_RX_GAIN_BIT(mode) (1U << (mode))
#define SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES \
    (SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_BOOSTED))
#define SOLAR_OS_RADIO_RX_GAIN_LNA_MODES \
    (SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_AUTO) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G1) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G2) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G3) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G4) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G5) | \
     SOLAR_OS_RADIO_RX_GAIN_BIT(SOLAR_OS_RADIO_RX_GAIN_G6))

const char *solar_os_radio_rx_gain_name(solar_os_radio_rx_gain_t gain);
bool solar_os_radio_rx_gain_parse(const char *name, uint8_t *gain);
void solar_os_radio_rx_gains_format(uint32_t modes, char *buffer, size_t length);

typedef enum {
    SOLAR_OS_RADIO_STATE_UNKNOWN,
    SOLAR_OS_RADIO_STATE_SLEEP,
    SOLAR_OS_RADIO_STATE_STANDBY,
    SOLAR_OS_RADIO_STATE_RX,
    SOLAR_OS_RADIO_STATE_TX,
} solar_os_radio_state_t;

/*
 * LoRa low-data-rate optimize. Both ends of a link must agree on it, and
 * implementations draw the line in different places, so a user that has to
 * match a particular peer says which. Otherwise the driver decides.
 */
typedef enum {
    SOLAR_OS_RADIO_LOW_DATA_RATE_AUTO = 0,
    SOLAR_OS_RADIO_LOW_DATA_RATE_OFF,
    SOLAR_OS_RADIO_LOW_DATA_RATE_ON,
} solar_os_radio_low_data_rate_t;

typedef struct {
    uint32_t frequency_hz;
    solar_os_radio_modulation_t modulation;
    uint32_t bitrate_bps;
    uint32_t deviation_hz;
    uint32_t rx_bandwidth_hz;
    uint8_t spreading_factor;
    uint8_t coding_rate_denominator;
    uint16_t preamble_len;
    uint8_t sync_word_len;
    uint8_t sync_word[SOLAR_OS_RADIO_SYNC_WORD_MAX];
    int8_t tx_power_dbm;
    bool crc_enabled;
    bool variable_length;
    uint16_t payload_length;
    bool has_node_id;
    uint8_t node_id;
    bool has_network_id;
    uint8_t network_id;
    /* Appended in former tail padding; zero preserves the driver's default. */
    uint8_t rx_gain;
    /* The last byte of former tail padding. */
    uint8_t low_data_rate; /* solar_os_radio_low_data_rate_t */
} solar_os_radio_config_t;

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
    solar_os_radio_state_t state;
    solar_os_radio_config_t config;
    bool has_rssi;
    int16_t rssi_dbm;
    bool has_snr;
    int16_t snr_db;
} solar_os_radio_status_t;

typedef struct {
    size_t len;
    uint8_t data[SOLAR_OS_RADIO_PACKET_MAX];
    bool has_source;
    uint16_t source;
    bool has_destination;
    uint16_t destination;
    bool has_rssi;
    int16_t rssi_dbm;
    bool has_snr;
    int16_t snr_db;
    bool crc_ok;
} solar_os_radio_packet_t;

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
    char name[SOLAR_OS_RADIO_NAME_MAX];
    char driver[SOLAR_OS_RADIO_DRIVER_MAX];
    char summary[SOLAR_OS_RADIO_SUMMARY_MAX];
    solar_os_radio_modulations_t modulations;
    solar_os_radio_features_t features;
    uint32_t rx_gain_modes;
    size_t max_packet_len;
    bool claimed;
    char owner[SOLAR_OS_RADIO_OWNER_MAX];
} solar_os_radio_info_t;

typedef struct {
    int index;
    uint32_t token;
} solar_os_radio_handle_t;

#define SOLAR_OS_RADIO_HANDLE_INIT { .index = -1, .token = 0 }

typedef struct {
    char name[SOLAR_OS_RADIO_PROFILE_NAME_MAX];
    bool builtin;
    solar_os_radio_config_t config;
} solar_os_radio_profile_t;

esp_err_t solar_os_radio_init(void);
esp_err_t solar_os_radio_register(const solar_os_radio_registration_t *registration);
esp_err_t solar_os_radio_unregister(const char *name);

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
