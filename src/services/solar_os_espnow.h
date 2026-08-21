#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define SOLAR_OS_ESPNOW_MAC_LEN 6U
#define SOLAR_OS_ESPNOW_FRAME_MTU 250U
#define SOLAR_OS_ESPNOW_PEER_MAX 19U
#define SOLAR_OS_ESPNOW_OWNER_MAX 32U

typedef enum {
    SOLAR_OS_ESPNOW_PHY_NORMAL = 0,
    SOLAR_OS_ESPNOW_PHY_LR500,
    SOLAR_OS_ESPNOW_PHY_LR250,
} solar_os_espnow_phy_t;

typedef struct {
    uint32_t link_id;
    uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN];
    bool configured;
    int8_t rssi;
    uint32_t last_seen_ms;
} solar_os_espnow_peer_t;

typedef struct {
    bool running;
    bool channel_auto;
    bool send_inflight;
    uint8_t channel;
    solar_os_espnow_phy_t phy;
    char owner[SOLAR_OS_ESPNOW_OWNER_MAX];
    size_t peer_count;
    size_t configured_peer_count;
    uint32_t tx_packets;
    uint32_t tx_errors;
    uint32_t rx_packets;
    uint32_t rx_dropped;
    uint32_t peer_conflicts;
    esp_err_t last_error;
} solar_os_espnow_status_t;

typedef struct {
    uint8_t source_mac[SOLAR_OS_ESPNOW_MAC_LEN];
    int8_t rssi;
    size_t len;
    uint8_t data[SOLAR_OS_ESPNOW_FRAME_MTU];
} solar_os_espnow_packet_t;

esp_err_t solar_os_espnow_start(const char *owner,
                                uint8_t requested_channel,
                                solar_os_espnow_phy_t phy);
esp_err_t solar_os_espnow_stop(const char *owner);
esp_err_t solar_os_espnow_prepare_sleep(void);
esp_err_t solar_os_espnow_resume(void);
void solar_os_espnow_get_status(solar_os_espnow_status_t *status);

size_t solar_os_espnow_peer_count(void);
bool solar_os_espnow_peer_get(size_t index, solar_os_espnow_peer_t *peer);
esp_err_t solar_os_espnow_peer_set(uint32_t link_id,
                                  const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN]);
esp_err_t solar_os_espnow_peer_remove(uint32_t link_id);
esp_err_t solar_os_espnow_peer_learn(uint32_t link_id,
                                    const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN],
                                    int8_t rssi,
                                    uint32_t now_ms);

esp_err_t solar_os_espnow_send(uint32_t link_destination,
                              const uint8_t *data,
                              size_t len);
esp_err_t solar_os_espnow_take_send_result(esp_err_t *result,
                                           uint32_t timeout_ms);
esp_err_t solar_os_espnow_receive(solar_os_espnow_packet_t *packet,
                                 uint32_t timeout_ms);

bool solar_os_espnow_parse_mac(const char *text,
                              uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN]);
bool solar_os_espnow_parse_phy(const char *text, solar_os_espnow_phy_t *phy);
const char *solar_os_espnow_phy_name(solar_os_espnow_phy_t phy);
void solar_os_espnow_format_mac(const uint8_t mac[SOLAR_OS_ESPNOW_MAC_LEN],
                               char *text,
                               size_t text_len);
