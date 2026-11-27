#pragma once

#include "esp_err.h"
#include "solar_os_espnow_types_v1.h"

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
