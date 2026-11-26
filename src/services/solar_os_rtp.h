#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_media.h"

typedef esp_err_t (*solar_os_rtp_packet_fn)(const uint8_t *packet,
                                            size_t packet_len,
                                            void *user);

esp_err_t solar_os_rtp_header_encode(const solar_os_rtp_header_t *header,
                                     uint8_t *packet,
                                     size_t packet_capacity);
esp_err_t solar_os_rtp_header_decode(const uint8_t *packet,
                                     size_t packet_len,
                                     solar_os_rtp_header_t *header,
                                     const uint8_t **payload,
                                     size_t *payload_len);
bool solar_os_rtp_sender_valid(const solar_os_rtp_sender_t *sender);

esp_err_t solar_os_rtp_l16_packetize(solar_os_rtp_sender_t *sender,
                                     const int16_t *samples,
                                     size_t frame_count,
                                     uint8_t channels,
                                     uint8_t *packet,
                                     size_t packet_capacity,
                                     solar_os_rtp_packet_fn write_packet,
                                     void *user);

esp_err_t solar_os_rtcp_sender_report(uint32_t ssrc,
                                      uint32_t ntp_seconds,
                                      uint32_t ntp_fraction,
                                      uint32_t rtp_timestamp,
                                      uint32_t packet_count,
                                      uint32_t octet_count,
                                      const char *cname,
                                      uint8_t *packet,
                                      size_t packet_capacity,
                                      size_t *packet_len);
esp_err_t solar_os_rtcp_bye(uint32_t ssrc,
                            uint8_t *packet,
                            size_t packet_capacity,
                            size_t *packet_len);
