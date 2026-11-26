#include "solar_os_rtp.h"

#include <string.h>

static uint16_t rtp_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static uint32_t rtp_read_u32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
        ((uint32_t)data[2] << 8U) | data[3];
}

static void rtp_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static void rtp_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24U);
    data[1] = (uint8_t)(value >> 16U);
    data[2] = (uint8_t)(value >> 8U);
    data[3] = (uint8_t)value;
}

esp_err_t solar_os_rtp_header_encode(const solar_os_rtp_header_t *header,
                                     uint8_t *packet,
                                     size_t packet_capacity)
{
    if (header == NULL || packet == NULL ||
        packet_capacity < SOLAR_OS_RTP_HEADER_BYTES ||
        header->payload_type > 127U) {
        return ESP_ERR_INVALID_ARG;
    }
    packet[0] = 0x80U;
    packet[1] = (header->marker ? 0x80U : 0U) | header->payload_type;
    rtp_write_u16(&packet[2], header->sequence);
    rtp_write_u32(&packet[4], header->timestamp);
    rtp_write_u32(&packet[8], header->ssrc);
    return ESP_OK;
}

esp_err_t solar_os_rtp_header_decode(const uint8_t *packet,
                                     size_t packet_len,
                                     solar_os_rtp_header_t *header,
                                     const uint8_t **payload,
                                     size_t *payload_len)
{
    if (packet == NULL || header == NULL || payload == NULL ||
        payload_len == NULL || packet_len < SOLAR_OS_RTP_HEADER_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((packet[0] >> 6U) != 2U || (packet[0] & 0x3fU) != 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *header = (solar_os_rtp_header_t) {
        .marker = (packet[1] & 0x80U) != 0U,
        .payload_type = packet[1] & 0x7fU,
        .sequence = rtp_read_u16(&packet[2]),
        .timestamp = rtp_read_u32(&packet[4]),
        .ssrc = rtp_read_u32(&packet[8]),
    };
    *payload = &packet[SOLAR_OS_RTP_HEADER_BYTES];
    *payload_len = packet_len - SOLAR_OS_RTP_HEADER_BYTES;
    return ESP_OK;
}

bool solar_os_rtp_sender_valid(const solar_os_rtp_sender_t *sender)
{
    return sender != NULL && sender->payload_type >= 96U &&
        sender->payload_type <= 127U &&
        sender->max_packet_bytes > SOLAR_OS_RTP_HEADER_BYTES &&
        sender->max_packet_bytes <= SOLAR_OS_MEDIA_RTP_PACKET_MAX;
}

esp_err_t solar_os_rtp_l16_packetize(solar_os_rtp_sender_t *sender,
                                     const int16_t *samples,
                                     size_t frame_count,
                                     uint8_t channels,
                                     uint8_t *packet,
                                     size_t packet_capacity,
                                     solar_os_rtp_packet_fn write_packet,
                                     void *user)
{
    if (!solar_os_rtp_sender_valid(sender) || samples == NULL ||
        frame_count == 0U || channels == 0U || channels > 2U ||
        frame_count > SIZE_MAX / ((size_t)channels * sizeof(*samples)) ||
        packet == NULL || packet_capacity < sender->max_packet_bytes ||
        write_packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t payload_capacity =
        sender->max_packet_bytes - SOLAR_OS_RTP_HEADER_BYTES;
    const size_t bytes_per_frame = (size_t)channels * sizeof(int16_t);
    const size_t frames_per_packet = payload_capacity / bytes_per_frame;
    if (frames_per_packet == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t frame_offset = 0U;
    const uint32_t initial_timestamp = sender->timestamp;
    while (frame_offset < frame_count) {
        size_t frames = frame_count - frame_offset;
        if (frames > frames_per_packet) {
            frames = frames_per_packet;
        }
        const solar_os_rtp_header_t header = {
            .marker = false,
            .payload_type = sender->payload_type,
            .sequence = sender->sequence,
            .timestamp = initial_timestamp + (uint32_t)frame_offset,
            .ssrc = sender->ssrc,
        };
        esp_err_t error = solar_os_rtp_header_encode(
            &header, packet, packet_capacity);
        if (error != ESP_OK) {
            return error;
        }
        uint8_t *output = &packet[SOLAR_OS_RTP_HEADER_BYTES];
        const size_t sample_offset = frame_offset * channels;
        const size_t sample_count = frames * channels;
        for (size_t i = 0U; i < sample_count; i++) {
            const uint16_t value = (uint16_t)samples[sample_offset + i];
            output[i * 2U] = (uint8_t)(value >> 8U);
            output[i * 2U + 1U] = (uint8_t)value;
        }
        sender->sequence++;
        error = write_packet(packet,
                             SOLAR_OS_RTP_HEADER_BYTES +
                                 sample_count * sizeof(int16_t),
                             user);
        frame_offset += frames;
        sender->timestamp = initial_timestamp + (uint32_t)frame_offset;
        if (error != ESP_OK) {
            return error;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_rtcp_sender_report(uint32_t ssrc,
                                      uint32_t ntp_seconds,
                                      uint32_t ntp_fraction,
                                      uint32_t rtp_timestamp,
                                      uint32_t packet_count,
                                      uint32_t octet_count,
                                      const char *cname,
                                      uint8_t *packet,
                                      size_t packet_capacity,
                                      size_t *packet_len)
{
    const size_t cname_len = cname != NULL ? strlen(cname) : 0U;
    if (cname_len == 0U || cname_len > 255U || packet == NULL ||
        packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t sdes_unpadded = 4U + 4U + 2U + cname_len + 1U;
    const size_t sdes_len = (sdes_unpadded + 3U) & ~(size_t)3U;
    const size_t total_len = 28U + sdes_len;
    if (packet_capacity < total_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(packet, 0, total_len);
    packet[0] = 0x80U;
    packet[1] = 200U;
    rtp_write_u16(&packet[2], 6U);
    rtp_write_u32(&packet[4], ssrc);
    rtp_write_u32(&packet[8], ntp_seconds);
    rtp_write_u32(&packet[12], ntp_fraction);
    rtp_write_u32(&packet[16], rtp_timestamp);
    rtp_write_u32(&packet[20], packet_count);
    rtp_write_u32(&packet[24], octet_count);

    uint8_t *sdes = &packet[28];
    sdes[0] = 0x81U;
    sdes[1] = 202U;
    rtp_write_u16(&sdes[2], (uint16_t)(sdes_len / 4U - 1U));
    rtp_write_u32(&sdes[4], ssrc);
    sdes[8] = 1U;
    sdes[9] = (uint8_t)cname_len;
    memcpy(&sdes[10], cname, cname_len);
    *packet_len = total_len;
    return ESP_OK;
}

esp_err_t solar_os_rtcp_bye(uint32_t ssrc,
                            uint8_t *packet,
                            size_t packet_capacity,
                            size_t *packet_len)
{
    if (packet == NULL || packet_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (packet_capacity < 8U) {
        return ESP_ERR_INVALID_SIZE;
    }
    packet[0] = 0x81U;
    packet[1] = 203U;
    rtp_write_u16(&packet[2], 1U);
    rtp_write_u32(&packet[4], ssrc);
    *packet_len = 8U;
    return ESP_OK;
}
