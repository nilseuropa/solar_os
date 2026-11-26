#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "solar_os_rtp.h"

typedef struct {
    uint8_t *buffer;
    size_t capacity;
    uint8_t payload_type;
    bool collecting;
    uint16_t expected_sequence;
    uint32_t timestamp;
    uint32_t ssrc;
    size_t scan_len;
    uint16_t width;
    uint16_t height;
    uint16_t restart_interval;
    uint8_t type;
    uint8_t quant_tables[SOLAR_OS_RTP_JPEG_QUANT_BYTES];
    uint32_t frames;
    uint32_t dropped_frames;
} solar_os_rtp_jpeg_receiver_t;

esp_err_t solar_os_rtp_jpeg_parse(const uint8_t *jpeg,
                                  size_t jpeg_len,
                                  solar_os_rtp_jpeg_view_t *view);
esp_err_t solar_os_rtp_jpeg_packetize(solar_os_rtp_sender_t *sender,
                                      const solar_os_rtp_jpeg_view_t *view,
                                      uint8_t *packet,
                                      size_t packet_capacity,
                                      solar_os_rtp_packet_fn write_packet,
                                      void *user);

esp_err_t solar_os_rtp_jpeg_receiver_init(
    solar_os_rtp_jpeg_receiver_t *receiver,
    uint8_t payload_type,
    uint8_t *buffer,
    size_t capacity);
void solar_os_rtp_jpeg_receiver_reset(
    solar_os_rtp_jpeg_receiver_t *receiver);
esp_err_t solar_os_rtp_jpeg_receiver_feed(
    solar_os_rtp_jpeg_receiver_t *receiver,
    const uint8_t *packet,
    size_t packet_len,
    solar_os_rtp_jpeg_frame_t *frame);
