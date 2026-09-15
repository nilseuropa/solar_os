#include "bhi260ap_codec.h"

#include <stddef.h>

#define STANDARD_GRAVITY_M_S2 9.80665F
#define DEGREES_TO_RADIANS 0.01745329251994329577F

static int16_t decode_i16(const uint8_t *data)
{
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

void bhi260ap_decode_acceleration(const uint8_t data[6], float output_m_s2[3])
{
    const float scale = STANDARD_GRAVITY_M_S2 / 4096.0F;
    for (size_t i = 0; i < 3U; i++) {
        output_m_s2[i] = (float)decode_i16(&data[i * 2U]) * scale;
    }
}

void bhi260ap_decode_angular_velocity(const uint8_t data[6],
                                      float output_rad_s[3])
{
    const float scale = (2000.0F / 32768.0F) * DEGREES_TO_RADIANS;
    for (size_t i = 0; i < 3U; i++) {
        output_rad_s[i] = (float)decode_i16(&data[i * 2U]) * scale;
    }
}

void bhi260ap_decode_quaternion(const uint8_t data[8], float output_wxyz[4])
{
    const float scale = 1.0F / 16384.0F;
    output_wxyz[0] = (float)decode_i16(&data[6]) * scale;
    output_wxyz[1] = (float)decode_i16(&data[0]) * scale;
    output_wxyz[2] = (float)decode_i16(&data[2]) * scale;
    output_wxyz[3] = (float)decode_i16(&data[4]) * scale;
}

uint64_t bhi260ap_timestamp_us(uint64_t sensor_timestamp)
{
    return (sensor_timestamp * 15625ULL) / 1000ULL;
}
