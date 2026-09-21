#include "mgc3130.h"

#include <string.h>

#define MGC3130_MESSAGE_SENSOR_DATA 0x91U
#define MGC3130_MESSAGE_SET_RUNTIME_PARAMETER 0xA2U
#define MGC3130_HEADER_SIZE 4U
#define MGC3130_SENSOR_FIXED_PAYLOAD_SIZE 4U

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}
static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8U) |
        ((uint32_t)data[2] << 16U) |
        ((uint32_t)data[3] << 24U);
}

static void write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8U);
    data[2] = (uint8_t)(value >> 16U);
    data[3] = (uint8_t)(value >> 24U);
}

static bool field_available(size_t offset, size_t field_size, size_t message_size)
{
    return offset <= message_size && field_size <= message_size - offset;
}

esp_err_t mgc3130_parse_sensor_frame(const uint8_t *frame,
                                     size_t frame_len,
                                     mgc3130_sample_t *sample)
{
    if (frame == NULL || sample == NULL || frame_len < MGC3130_HEADER_SIZE) {
        return ESP_ERR_INVALID_ARG;
    }
    const size_t message_size = frame[0];
    if (message_size < MGC3130_HEADER_SIZE + MGC3130_SENSOR_FIXED_PAYLOAD_SIZE ||
        message_size > frame_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (frame[3] != MGC3130_MESSAGE_SENSOR_DATA) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    memset(sample, 0, sizeof(*sample));
    sample->sequence = frame[2];
    sample->data_mask = read_le16(&frame[4]);
    sample->timestamp = frame[6];
    sample->system_info = frame[7];

    size_t offset = MGC3130_HEADER_SIZE + MGC3130_SENSOR_FIXED_PAYLOAD_SIZE;
    if ((sample->data_mask & MGC3130_DATA_DSP_STATUS) != 0U) {
        if (!field_available(offset, 2U, message_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        sample->has_dsp_status = true;
        sample->dsp_status = read_le16(&frame[offset]);
        offset += 2U;
    }
    if ((sample->data_mask & MGC3130_DATA_GESTURE) != 0U) {
        if (!field_available(offset, 4U, message_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        sample->has_gesture = true;
        sample->gesture_info = read_le32(&frame[offset]);
        offset += 4U;
    }
    if ((sample->data_mask & MGC3130_DATA_TOUCH) != 0U) {
        if (!field_available(offset, 4U, message_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        sample->has_touch = true;
        sample->touch_info = read_le32(&frame[offset]);
        offset += 4U;
    }
    if ((sample->data_mask & MGC3130_DATA_AIRWHEEL) != 0U) {
        if (!field_available(offset, 2U, message_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        sample->has_airwheel = true;
        sample->airwheel = frame[offset];
        offset += 2U;
    }
    if ((sample->data_mask & MGC3130_DATA_POSITION) != 0U) {
        if (!field_available(offset, 6U, message_size)) {
            return ESP_ERR_INVALID_SIZE;
        }
        sample->has_position = true;
        sample->x = read_le16(&frame[offset]);
        sample->y = read_le16(&frame[offset + 2U]);
        sample->z = read_le16(&frame[offset + 4U]);
        offset += 6U;
    }
    if ((sample->data_mask & MGC3130_DATA_NOISE_POWER) != 0U &&
        !field_available(offset, 4U, message_size)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t mgc3130_build_runtime_parameter(uint16_t parameter,
                                          uint32_t value,
                                          uint32_t mask,
                                          uint8_t sequence,
                                          uint8_t frame[MGC3130_RUNTIME_FRAME_SIZE])
{
    if (frame == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(frame, 0, MGC3130_RUNTIME_FRAME_SIZE);
    frame[0] = MGC3130_RUNTIME_FRAME_SIZE;
    frame[2] = sequence;
    frame[3] = MGC3130_MESSAGE_SET_RUNTIME_PARAMETER;
    write_le16(&frame[4], parameter);
    write_le32(&frame[8], value);
    write_le32(&frame[12], mask);
    return ESP_OK;
}
