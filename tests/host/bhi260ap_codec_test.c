#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "bhi260ap_codec.h"

static void assert_near(float actual, float expected, float tolerance)
{
    assert(fabsf(actual - expected) <= tolerance);
}

int main(void)
{
    const uint8_t acceleration[] = {
        0x00, 0x10,
        0x00, 0xf0,
        0x00, 0x08,
    };
    float vector[3];
    bhi260ap_decode_acceleration(acceleration, vector);
    assert_near(vector[0], 9.80665F, 0.0001F);
    assert_near(vector[1], -9.80665F, 0.0001F);
    assert_near(vector[2], 4.903325F, 0.0001F);

    const uint8_t angular_velocity[] = {
        0x00, 0x40,
        0x00, 0xc0,
        0x00, 0x20,
    };
    bhi260ap_decode_angular_velocity(angular_velocity, vector);
    assert_near(vector[0], 17.4532925F, 0.0001F);
    assert_near(vector[1], -17.4532925F, 0.0001F);
    assert_near(vector[2], 8.7266463F, 0.0001F);

    const uint8_t quaternion[] = {
        0x00, 0x10,
        0x00, 0xe0,
        0x00, 0x20,
        0x00, 0x40,
    };
    float rotation[4];
    bhi260ap_decode_quaternion(quaternion, rotation);
    assert_near(rotation[0], 1.0F, 0.0001F);
    assert_near(rotation[1], 0.25F, 0.0001F);
    assert_near(rotation[2], -0.5F, 0.0001F);
    assert_near(rotation[3], 0.5F, 0.0001F);

    assert(bhi260ap_timestamp_us(64U) == 1000U);
    puts("BHI260AP codec tests: ok");
    return 0;
}
