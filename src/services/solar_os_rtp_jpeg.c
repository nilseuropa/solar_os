#include "solar_os_rtp_jpeg.h"

#include <string.h>

#define JPEG_MARKER_PREFIX 0xffU
#define JPEG_MARKER_SOF0 0xc0U
#define JPEG_MARKER_DHT 0xc4U
#define JPEG_MARKER_SOI 0xd8U
#define JPEG_MARKER_EOI 0xd9U
#define JPEG_MARKER_SOS 0xdaU
#define JPEG_MARKER_DQT 0xdbU
#define JPEG_MARKER_DRI 0xddU
#define JPEG_MARKER_APP0 0xe0U

static const uint8_t jpeg_dc_luma_counts[16] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
};
static const uint8_t jpeg_dc_values[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
};
static const uint8_t jpeg_ac_luma_counts[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d,
};
static const uint8_t jpeg_ac_luma_values[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
static const uint8_t jpeg_dc_chroma_counts[16] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0,
};
static const uint8_t jpeg_ac_chroma_counts[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77,
};
static const uint8_t jpeg_ac_chroma_values[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

typedef struct {
    uint8_t info;
    const uint8_t *counts;
    const uint8_t *values;
    size_t value_count;
} jpeg_huffman_table_t;

static const jpeg_huffman_table_t jpeg_huffman_tables[] = {
    {0x00U, jpeg_dc_luma_counts, jpeg_dc_values, sizeof(jpeg_dc_values)},
    {0x10U, jpeg_ac_luma_counts, jpeg_ac_luma_values,
     sizeof(jpeg_ac_luma_values)},
    {0x01U, jpeg_dc_chroma_counts, jpeg_dc_values, sizeof(jpeg_dc_values)},
    {0x11U, jpeg_ac_chroma_counts, jpeg_ac_chroma_values,
     sizeof(jpeg_ac_chroma_values)},
};

static uint16_t jpeg_read_u16(const uint8_t *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8U) | data[1]);
}

static void jpeg_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value >> 8U);
    data[1] = (uint8_t)value;
}

static bool jpeg_view_valid(const solar_os_rtp_jpeg_view_t *view)
{
    const bool type_valid = view != NULL &&
        (view->type == 0U || view->type == 1U);
    return type_valid && view->scan != NULL && view->scan_len > 0U &&
        view->scan_len < (1U << 24U) && view->width > 0U &&
        view->height > 0U && view->width <= 2040U &&
        view->height <= 2040U && (view->width % 8U) == 0U &&
        (view->height % 8U) == 0U;
}

static const jpeg_huffman_table_t *jpeg_huffman_table(uint8_t info,
                                                       size_t *index)
{
    for (size_t i = 0U;
         i < sizeof(jpeg_huffman_tables) / sizeof(jpeg_huffman_tables[0]);
         i++) {
        if (jpeg_huffman_tables[i].info == info) {
            if (index != NULL) {
                *index = i;
            }
            return &jpeg_huffman_tables[i];
        }
    }
    return NULL;
}

static esp_err_t jpeg_parse_dqt(const uint8_t *data,
                                size_t length,
                                solar_os_rtp_jpeg_view_t *view,
                                uint8_t *seen)
{
    size_t offset = 0U;
    while (offset < length) {
        const uint8_t info = data[offset++];
        const uint8_t precision = info >> 4U;
        const uint8_t id = info & 0x0fU;
        if (precision != 0U || id > 1U || length - offset < 64U) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        memcpy(&view->quant_tables[(size_t)id * 64U], &data[offset], 64U);
        *seen |= (uint8_t)(1U << id);
        offset += 64U;
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t jpeg_parse_dht(const uint8_t *data,
                                size_t length,
                                uint8_t *seen)
{
    size_t offset = 0U;
    while (offset < length) {
        if (length - offset < 17U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t table_index = 0U;
        const jpeg_huffman_table_t *standard =
            jpeg_huffman_table(data[offset++], &table_index);
        if (standard == NULL ||
            memcmp(&data[offset], standard->counts, 16U) != 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        size_t value_count = 0U;
        for (size_t i = 0U; i < 16U; i++) {
            value_count += data[offset + i];
        }
        offset += 16U;
        if (value_count != standard->value_count ||
            length - offset < value_count ||
            memcmp(&data[offset], standard->values, value_count) != 0) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        *seen |= (uint8_t)(1U << table_index);
        offset += value_count;
    }
    return offset == length ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t jpeg_parse_sof0(const uint8_t *data,
                                 size_t length,
                                 solar_os_rtp_jpeg_view_t *view)
{
    if (length != 15U || data[0] != 8U || data[5] != 3U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint16_t height = jpeg_read_u16(&data[1]);
    const uint16_t width = jpeg_read_u16(&data[3]);
    if (width == 0U || height == 0U || width > 2040U || height > 2040U ||
        (width % 8U) != 0U || (height % 8U) != 0U ||
        data[6] != 1U || data[8] != 0U || data[9] != 2U ||
        data[10] != 0x11U || data[11] != 1U || data[12] != 3U ||
        data[13] != 0x11U || data[14] != 1U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (data[7] == 0x21U) {
        view->type = 0U;
    } else if (data[7] == 0x22U) {
        view->type = 1U;
    } else {
        return ESP_ERR_NOT_SUPPORTED;
    }
    view->width = width;
    view->height = height;
    return ESP_OK;
}

static esp_err_t jpeg_parse_sos(const uint8_t *data, size_t length)
{
    static const uint8_t expected[] = {
        3U, 1U, 0x00U, 2U, 0x11U, 3U, 0x11U, 0U, 63U, 0U,
    };
    return length == sizeof(expected) &&
        memcmp(data, expected, sizeof(expected)) == 0 ?
        ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t jpeg_find_scan_end(const uint8_t *jpeg,
                                    size_t jpeg_len,
                                    size_t scan_start,
                                    bool restart_allowed,
                                    size_t *scan_len)
{
    for (size_t i = scan_start; i + 1U < jpeg_len; i++) {
        if (jpeg[i] != JPEG_MARKER_PREFIX) {
            continue;
        }
        size_t marker_at = i;
        while (i + 1U < jpeg_len && jpeg[i + 1U] == JPEG_MARKER_PREFIX) {
            i++;
        }
        if (i + 1U >= jpeg_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t marker = jpeg[i + 1U];
        if (marker == 0U) {
            i++;
            continue;
        }
        if (marker >= 0xd0U && marker <= 0xd7U) {
            if (!restart_allowed) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            i++;
            continue;
        }
        if (marker == JPEG_MARKER_EOI) {
            *scan_len = marker_at - scan_start;
            return *scan_len > 0U ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t solar_os_rtp_jpeg_parse(const uint8_t *jpeg,
                                  size_t jpeg_len,
                                  solar_os_rtp_jpeg_view_t *view)
{
    if (jpeg == NULL || view == NULL || jpeg_len < 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(view, 0, sizeof(*view));
    if (jpeg[0] != JPEG_MARKER_PREFIX || jpeg[1] != JPEG_MARKER_SOI) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t quant_seen = 0U;
    uint8_t huffman_seen = 0U;
    bool sof_seen = false;
    size_t offset = 2U;
    while (offset + 1U < jpeg_len) {
        if (jpeg[offset] != JPEG_MARKER_PREFIX) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        while (offset < jpeg_len && jpeg[offset] == JPEG_MARKER_PREFIX) {
            offset++;
        }
        if (offset >= jpeg_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t marker = jpeg[offset++];
        if (marker == JPEG_MARKER_EOI || marker == JPEG_MARKER_SOI ||
            (marker >= 0xd0U && marker <= 0xd7U)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (offset + 2U > jpeg_len) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint16_t segment_length = jpeg_read_u16(&jpeg[offset]);
        offset += 2U;
        if (segment_length < 2U ||
            (size_t)segment_length - 2U > jpeg_len - offset) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const size_t payload_length = (size_t)segment_length - 2U;
        const uint8_t *payload = &jpeg[offset];
        esp_err_t error = ESP_OK;
        if (marker == JPEG_MARKER_DQT) {
            error = jpeg_parse_dqt(payload, payload_length, view, &quant_seen);
        } else if (marker == JPEG_MARKER_DHT) {
            error = jpeg_parse_dht(payload, payload_length, &huffman_seen);
        } else if (marker == JPEG_MARKER_SOF0) {
            if (sof_seen) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            error = jpeg_parse_sof0(payload, payload_length, view);
            sof_seen = error == ESP_OK;
        } else if (marker == JPEG_MARKER_DRI) {
            if (payload_length != 2U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            view->restart_interval = jpeg_read_u16(payload);
            if (view->restart_interval == 0U) {
                return ESP_ERR_INVALID_RESPONSE;
            }
        } else if (marker == JPEG_MARKER_SOS) {
            error = jpeg_parse_sos(payload, payload_length);
            if (error != ESP_OK || !sof_seen || quant_seen != 0x03U ||
                (huffman_seen != 0U && huffman_seen != 0x0fU)) {
                return error != ESP_OK ? error : ESP_ERR_NOT_SUPPORTED;
            }
            const size_t scan_start = offset + payload_length;
            error = jpeg_find_scan_end(
                jpeg,
                jpeg_len,
                scan_start,
                view->restart_interval != 0U,
                &view->scan_len);
            if (error != ESP_OK) {
                return error;
            }
            view->scan = &jpeg[scan_start];
            return jpeg_view_valid(view) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
        } else if ((marker >= 0xc1U && marker <= 0xcfU) &&
                   marker != JPEG_MARKER_DHT) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (error != ESP_OK) {
            return error;
        }
        offset += payload_length;
    }
    return ESP_ERR_INVALID_RESPONSE;
}

static void rtp_jpeg_write_offset(uint8_t *data, size_t offset)
{
    data[0] = (uint8_t)(offset >> 16U);
    data[1] = (uint8_t)(offset >> 8U);
    data[2] = (uint8_t)offset;
}

static size_t rtp_jpeg_read_offset(const uint8_t *data)
{
    return ((size_t)data[0] << 16U) | ((size_t)data[1] << 8U) | data[2];
}

esp_err_t solar_os_rtp_jpeg_packetize(solar_os_rtp_sender_t *sender,
                                      const solar_os_rtp_jpeg_view_t *view,
                                      uint8_t *packet,
                                      size_t packet_capacity,
                                      solar_os_rtp_packet_fn write_packet,
                                      void *user)
{
    if (!solar_os_rtp_sender_valid(sender) || !jpeg_view_valid(view) ||
        packet == NULL || packet_capacity < sender->max_packet_bytes ||
        write_packet == NULL ||
        view->scan_len > SOLAR_OS_MEDIA_VIDEO_FRAME_MAX - SOLAR_OS_RTP_JPEG_HEADER_RESERVE - 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    const bool restart = view->restart_interval != 0U;
    const size_t restart_bytes = restart ? 4U : 0U;
    size_t fragment_offset = 0U;
    while (fragment_offset < view->scan_len) {
        const bool first = fragment_offset == 0U;
        const size_t quant_bytes = first ? 4U + SOLAR_OS_RTP_JPEG_QUANT_BYTES : 0U;
        const size_t headers = SOLAR_OS_RTP_HEADER_BYTES +
            SOLAR_OS_RTP_JPEG_HEADER_BYTES + restart_bytes + quant_bytes;
        if (headers >= sender->max_packet_bytes) {
            return ESP_ERR_INVALID_SIZE;
        }
        size_t fragment_len = view->scan_len - fragment_offset;
        const size_t payload_capacity = sender->max_packet_bytes - headers;
        if (fragment_len > payload_capacity) {
            fragment_len = payload_capacity;
        }
        const bool last = fragment_offset + fragment_len == view->scan_len;
        const solar_os_rtp_header_t rtp = {
            .marker = last,
            .payload_type = sender->payload_type,
            .sequence = sender->sequence,
            .timestamp = sender->timestamp,
            .ssrc = sender->ssrc,
        };
        esp_err_t error = solar_os_rtp_header_encode(
            &rtp, packet, packet_capacity);
        if (error != ESP_OK) {
            return error;
        }
        uint8_t *header = &packet[SOLAR_OS_RTP_HEADER_BYTES];
        header[0] = 0U;
        rtp_jpeg_write_offset(&header[1], fragment_offset);
        header[4] = view->type + (restart ? 64U : 0U);
        header[5] = 255U;
        header[6] = (uint8_t)(view->width / 8U);
        header[7] = (uint8_t)(view->height / 8U);
        size_t output_offset = SOLAR_OS_RTP_HEADER_BYTES +
            SOLAR_OS_RTP_JPEG_HEADER_BYTES;
        if (restart) {
            jpeg_write_u16(&packet[output_offset], view->restart_interval);
            packet[output_offset + 2U] = 0xffU;
            packet[output_offset + 3U] = 0xffU;
            output_offset += 4U;
        }
        if (first) {
            packet[output_offset] = 0U;
            packet[output_offset + 1U] = 0U;
            jpeg_write_u16(&packet[output_offset + 2U],
                           SOLAR_OS_RTP_JPEG_QUANT_BYTES);
            memcpy(&packet[output_offset + 4U],
                   view->quant_tables,
                   SOLAR_OS_RTP_JPEG_QUANT_BYTES);
            output_offset += 4U + SOLAR_OS_RTP_JPEG_QUANT_BYTES;
        }
        memcpy(&packet[output_offset],
               &view->scan[fragment_offset],
               fragment_len);
        sender->sequence++;
        error = write_packet(packet, output_offset + fragment_len, user);
        if (error != ESP_OK) {
            return error;
        }
        fragment_offset += fragment_len;
    }
    return ESP_OK;
}

typedef struct {
    uint8_t data[SOLAR_OS_RTP_JPEG_HEADER_RESERVE];
    size_t length;
} jpeg_header_builder_t;

static bool jpeg_header_write(jpeg_header_builder_t *builder,
                              const void *data,
                              size_t length)
{
    if (length > sizeof(builder->data) - builder->length) {
        return false;
    }
    memcpy(&builder->data[builder->length], data, length);
    builder->length += length;
    return true;
}

static bool jpeg_header_byte(jpeg_header_builder_t *builder, uint8_t value)
{
    return jpeg_header_write(builder, &value, 1U);
}

static bool jpeg_header_u16(jpeg_header_builder_t *builder, uint16_t value)
{
    const uint8_t data[2] = {(uint8_t)(value >> 8U), (uint8_t)value};
    return jpeg_header_write(builder, data, sizeof(data));
}

static bool jpeg_header_marker(jpeg_header_builder_t *builder, uint8_t marker)
{
    return jpeg_header_byte(builder, JPEG_MARKER_PREFIX) &&
        jpeg_header_byte(builder, marker);
}

static bool jpeg_header_dht(jpeg_header_builder_t *builder,
                            const jpeg_huffman_table_t *table)
{
    return jpeg_header_marker(builder, JPEG_MARKER_DHT) &&
        jpeg_header_u16(builder, (uint16_t)(19U + table->value_count)) &&
        jpeg_header_byte(builder, table->info) &&
        jpeg_header_write(builder, table->counts, 16U) &&
        jpeg_header_write(builder, table->values, table->value_count);
}

static esp_err_t jpeg_build_header(const solar_os_rtp_jpeg_receiver_t *receiver,
                                   jpeg_header_builder_t *builder)
{
    memset(builder, 0, sizeof(*builder));
    static const uint8_t jfif[] = {
        'J', 'F', 'I', 'F', 0U, 1U, 1U, 0U, 0U, 1U, 0U, 1U, 0U, 0U,
    };
    if (!jpeg_header_marker(builder, JPEG_MARKER_SOI) ||
        !jpeg_header_marker(builder, JPEG_MARKER_APP0) ||
        !jpeg_header_u16(builder, 16U) ||
        !jpeg_header_write(builder, jfif, sizeof(jfif)) ||
        !jpeg_header_marker(builder, JPEG_MARKER_DQT) ||
        !jpeg_header_u16(builder, 132U) ||
        !jpeg_header_byte(builder, 0U) ||
        !jpeg_header_write(builder, receiver->quant_tables, 64U) ||
        !jpeg_header_byte(builder, 1U) ||
        !jpeg_header_write(builder, &receiver->quant_tables[64], 64U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (receiver->restart_interval != 0U &&
        (!jpeg_header_marker(builder, JPEG_MARKER_DRI) ||
         !jpeg_header_u16(builder, 4U) ||
         !jpeg_header_u16(builder, receiver->restart_interval))) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!jpeg_header_marker(builder, JPEG_MARKER_SOF0) ||
        !jpeg_header_u16(builder, 17U) ||
        !jpeg_header_byte(builder, 8U) ||
        !jpeg_header_u16(builder, receiver->height) ||
        !jpeg_header_u16(builder, receiver->width) ||
        !jpeg_header_byte(builder, 3U) ||
        !jpeg_header_byte(builder, 1U) ||
        !jpeg_header_byte(builder, receiver->type == 0U ? 0x21U : 0x22U) ||
        !jpeg_header_byte(builder, 0U) ||
        !jpeg_header_byte(builder, 2U) ||
        !jpeg_header_byte(builder, 0x11U) ||
        !jpeg_header_byte(builder, 1U) ||
        !jpeg_header_byte(builder, 3U) ||
        !jpeg_header_byte(builder, 0x11U) ||
        !jpeg_header_byte(builder, 1U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0U;
         i < sizeof(jpeg_huffman_tables) / sizeof(jpeg_huffman_tables[0]);
         i++) {
        if (!jpeg_header_dht(builder, &jpeg_huffman_tables[i])) {
            return ESP_ERR_INVALID_SIZE;
        }
    }
    static const uint8_t scan_header[] = {
        3U, 1U, 0x00U, 2U, 0x11U, 3U, 0x11U, 0U, 63U, 0U,
    };
    if (!jpeg_header_marker(builder, JPEG_MARKER_SOS) ||
        !jpeg_header_u16(builder, 12U) ||
        !jpeg_header_write(builder, scan_header, sizeof(scan_header))) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static void jpeg_receiver_drop(solar_os_rtp_jpeg_receiver_t *receiver)
{
    if (receiver->collecting) {
        receiver->dropped_frames++;
    }
    receiver->collecting = false;
    receiver->scan_len = 0U;
}

esp_err_t solar_os_rtp_jpeg_receiver_init(
    solar_os_rtp_jpeg_receiver_t *receiver,
    uint8_t payload_type,
    uint8_t *buffer,
    size_t capacity)
{
    if (receiver == NULL || (payload_type != 26U && payload_type < 96U) || payload_type > 127U ||
        buffer == NULL || capacity < SOLAR_OS_RTP_JPEG_HEADER_RESERVE + 3U ||
        capacity > SOLAR_OS_MEDIA_VIDEO_FRAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(receiver, 0, sizeof(*receiver));
    receiver->buffer = buffer;
    receiver->capacity = capacity;
    receiver->payload_type = payload_type;
    return ESP_OK;
}

void solar_os_rtp_jpeg_receiver_reset(
    solar_os_rtp_jpeg_receiver_t *receiver)
{
    if (receiver != NULL) {
        receiver->collecting = false;
        receiver->scan_len = 0U;
    }
}

static esp_err_t jpeg_receiver_start(
    solar_os_rtp_jpeg_receiver_t *receiver,
    const solar_os_rtp_header_t *rtp,
    const uint8_t *header,
    const uint8_t **scan,
    size_t *scan_len)
{
    const uint8_t wire_type = header[4];
    const bool restart = wire_type == 64U || wire_type == 65U;
    if (wire_type != 0U && wire_type != 1U && !restart) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    size_t offset = SOLAR_OS_RTP_JPEG_HEADER_BYTES;
    uint16_t restart_interval = 0U;
    if (restart) {
        if (*scan_len < offset + 4U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        restart_interval = jpeg_read_u16(&header[offset]);
        if (restart_interval == 0U) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset += 4U;
    }
    if (header[5] != 255U || *scan_len < offset + 4U ||
        header[offset] != 0U || header[offset + 1U] != 0U ||
        jpeg_read_u16(&header[offset + 2U]) !=
            SOLAR_OS_RTP_JPEG_QUANT_BYTES ||
        *scan_len < offset + 4U + SOLAR_OS_RTP_JPEG_QUANT_BYTES ||
        header[6] == 0U || header[7] == 0U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    memcpy(receiver->quant_tables,
           &header[offset + 4U],
           SOLAR_OS_RTP_JPEG_QUANT_BYTES);
    offset += 4U + SOLAR_OS_RTP_JPEG_QUANT_BYTES;
    receiver->collecting = true;
    receiver->expected_sequence = rtp->sequence;
    receiver->timestamp = rtp->timestamp;
    receiver->ssrc = rtp->ssrc;
    receiver->scan_len = 0U;
    receiver->width = (uint16_t)header[6] * 8U;
    receiver->height = (uint16_t)header[7] * 8U;
    receiver->restart_interval = restart_interval;
    receiver->type = wire_type & 0x3fU;
    *scan = &header[offset];
    *scan_len -= offset;
    return ESP_OK;
}

esp_err_t solar_os_rtp_jpeg_receiver_feed(
    solar_os_rtp_jpeg_receiver_t *receiver,
    const uint8_t *packet,
    size_t packet_len,
    solar_os_rtp_jpeg_frame_t *frame)
{
    if (receiver == NULL || receiver->buffer == NULL || frame == NULL ||
        packet == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(frame, 0, sizeof(*frame));
    solar_os_rtp_header_t rtp;
    const uint8_t *payload = NULL;
    size_t payload_len = 0U;
    esp_err_t error = solar_os_rtp_header_decode(
        packet, packet_len, &rtp, &payload, &payload_len);
    if (error != ESP_OK) {
        jpeg_receiver_drop(receiver);
        return error;
    }
    if (rtp.payload_type != receiver->payload_type ||
        payload_len < SOLAR_OS_RTP_JPEG_HEADER_BYTES) {
        jpeg_receiver_drop(receiver);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const size_t fragment_offset = rtp_jpeg_read_offset(&payload[1]);
    const bool new_frame = !receiver->collecting ||
        rtp.ssrc != receiver->ssrc || rtp.timestamp != receiver->timestamp;
    if (new_frame) {
        jpeg_receiver_drop(receiver);
        if (fragment_offset != 0U) {
            receiver->dropped_frames++;
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint8_t *scan = payload;
        size_t scan_len = payload_len;
        error = jpeg_receiver_start(receiver, &rtp, payload, &scan, &scan_len);
        if (error != ESP_OK) {
            jpeg_receiver_drop(receiver);
            return error;
        }
        payload = scan;
        payload_len = scan_len;
    } else {
        const uint8_t wire_type = payload[4];
        const bool restart = wire_type == 64U || wire_type == 65U;
        const size_t header_len = SOLAR_OS_RTP_JPEG_HEADER_BYTES +
            (restart ? 4U : 0U);
        if (rtp.sequence != (uint16_t)(receiver->expected_sequence + 1U) ||
            fragment_offset != receiver->scan_len ||
            wire_type != receiver->type +
                (receiver->restart_interval != 0U ? 64U : 0U) ||
            payload[5] != 255U ||
            (uint16_t)payload[6] * 8U != receiver->width ||
            (uint16_t)payload[7] * 8U != receiver->height ||
            payload_len < header_len) {
            jpeg_receiver_drop(receiver);
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (restart &&
            jpeg_read_u16(&payload[SOLAR_OS_RTP_JPEG_HEADER_BYTES]) !=
                receiver->restart_interval) {
            jpeg_receiver_drop(receiver);
            return ESP_ERR_INVALID_RESPONSE;
        }
        payload += header_len;
        payload_len -= header_len;
    }
    if (payload_len == 0U ||
        payload_len > receiver->capacity - SOLAR_OS_RTP_JPEG_HEADER_RESERVE -
            receiver->scan_len - 2U) {
        jpeg_receiver_drop(receiver);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&receiver->buffer[SOLAR_OS_RTP_JPEG_HEADER_RESERVE +
                             receiver->scan_len],
           payload,
           payload_len);
    receiver->scan_len += payload_len;
    receiver->expected_sequence = rtp.sequence;
    if (!rtp.marker) {
        return ESP_OK;
    }

    jpeg_header_builder_t builder;
    error = jpeg_build_header(receiver, &builder);
    if (error != ESP_OK || builder.length > SOLAR_OS_RTP_JPEG_HEADER_RESERVE) {
        jpeg_receiver_drop(receiver);
        return error != ESP_OK ? error : ESP_ERR_INVALID_SIZE;
    }
    const size_t header_offset =
        SOLAR_OS_RTP_JPEG_HEADER_RESERVE - builder.length;
    memcpy(&receiver->buffer[header_offset], builder.data, builder.length);
    receiver->buffer[SOLAR_OS_RTP_JPEG_HEADER_RESERVE + receiver->scan_len] =
        JPEG_MARKER_PREFIX;
    receiver->buffer[SOLAR_OS_RTP_JPEG_HEADER_RESERVE +
                     receiver->scan_len + 1U] = JPEG_MARKER_EOI;
    *frame = (solar_os_rtp_jpeg_frame_t) {
        .data = &receiver->buffer[header_offset],
        .length = builder.length + receiver->scan_len + 2U,
        .width = receiver->width,
        .height = receiver->height,
        .timestamp = receiver->timestamp,
        .ssrc = receiver->ssrc,
    };
    receiver->frames++;
    receiver->collecting = false;
    receiver->scan_len = 0U;
    return ESP_OK;
}
