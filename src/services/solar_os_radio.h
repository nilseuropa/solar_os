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
} solar_os_radio_feature_t;

typedef enum {
    SOLAR_OS_RADIO_STATE_UNKNOWN,
    SOLAR_OS_RADIO_STATE_SLEEP,
    SOLAR_OS_RADIO_STATE_STANDBY,
    SOLAR_OS_RADIO_STATE_RX,
    SOLAR_OS_RADIO_STATE_TX,
} solar_os_radio_state_t;

typedef struct {
    uint32_t frequency_hz;
    solar_os_radio_modulation_t modulation;
    uint32_t bitrate_bps;
    uint32_t deviation_hz;
    uint32_t rx_bandwidth_hz;
    uint16_t preamble_len;
    uint8_t sync_word_len;
    uint8_t sync_word[SOLAR_OS_RADIO_SYNC_WORD_MAX];
    int8_t tx_power_dbm;
    bool crc_enabled;
    bool variable_length;
    bool has_node_id;
    uint8_t node_id;
    bool has_network_id;
    uint8_t network_id;
} solar_os_radio_config_t;

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
    esp_err_t (*receive)(void *ctx, solar_os_radio_packet_t *packet, uint32_t timeout_ms);
} solar_os_radio_ops_t;

typedef struct {
    const char *name;
    const char *driver;
    const char *summary;
    solar_os_radio_modulations_t modulations;
    solar_os_radio_features_t features;
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
    size_t max_packet_len;
} solar_os_radio_info_t;

esp_err_t solar_os_radio_init(void);
esp_err_t solar_os_radio_register(const solar_os_radio_registration_t *registration);
esp_err_t solar_os_radio_unregister(const char *name);

size_t solar_os_radio_count(void);
bool solar_os_radio_get(size_t index, solar_os_radio_info_t *info);
esp_err_t solar_os_radio_get_info(const char *name, solar_os_radio_info_t *info);
esp_err_t solar_os_radio_get_status(const char *name, solar_os_radio_status_t *status);

esp_err_t solar_os_radio_configure(const char *name, const solar_os_radio_config_t *config);
esp_err_t solar_os_radio_set_state(const char *name, solar_os_radio_state_t state);
esp_err_t solar_os_radio_send(const char *name,
                              const solar_os_radio_packet_t *packet,
                              uint32_t timeout_ms);
esp_err_t solar_os_radio_receive(const char *name,
                                 solar_os_radio_packet_t *packet,
                                 uint32_t timeout_ms);

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
