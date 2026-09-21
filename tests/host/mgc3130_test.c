#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "mgc3130.h"

static void test_runtime_parameter(void)
{
    uint8_t frame[MGC3130_RUNTIME_FRAME_SIZE];
    assert(mgc3130_build_runtime_parameter(0x0085U,
                                           0xffc001ffUL,
                                           0xffc001ffUL,
                                           7U,
                                           frame) == ESP_OK);
    const uint8_t expected[] = {
        0x10, 0x00, 0x07, 0xa2, 0x85, 0x00, 0x00, 0x00,
        0xff, 0x01, 0xc0, 0xff, 0xff, 0x01, 0xc0, 0xff,
    };
    assert(memcmp(frame, expected, sizeof(expected)) == 0);
}

static void test_full_sensor_frame(void)
{
    const uint8_t frame[MGC3130_SENSOR_FRAME_SIZE] = {
        26, 0, 9, 0x91,
        0x1f, 0x00, 0x42, 0x83,
        0x24, 0x55,
        0x42, 0x10, 0x00, 0x28,
        0x11, 0x24, 0x5a, 0x00,
        0x7f, 0x00,
        0x34, 0x12, 0x78, 0x56, 0xbc, 0x9a,
    };
    mgc3130_sample_t sample;
    assert(mgc3130_parse_sensor_frame(frame, sizeof(frame), &sample) == ESP_OK);
    assert(sample.sequence == 9U);
    assert(sample.timestamp == 0x42U);
    assert(sample.system_info == 0x83U);
    assert(sample.data_mask == 0x001fU);
    assert(sample.has_dsp_status && sample.dsp_status == 0x5524U);
    assert(sample.has_gesture && sample.gesture_info == 0x28001042UL);
    assert(sample.has_touch && sample.touch_info == 0x005a2411UL);
    assert(sample.has_airwheel && sample.airwheel == 0x7fU);
    assert(sample.has_position);
    assert(sample.x == 0x1234U);
    assert(sample.y == 0x5678U);
    assert(sample.z == 0x9abcU);
}

static void test_variable_fields_and_errors(void)
{
    const uint8_t position_only[] = {
        14, 0, 1, 0x91,
        MGC3130_DATA_POSITION, 0, 2, MGC3130_SYSTEM_POSITION_VALID,
        1, 0, 2, 0, 3, 0,
    };
    mgc3130_sample_t sample;
    assert(mgc3130_parse_sensor_frame(position_only,
                                      sizeof(position_only),
                                      &sample) == ESP_OK);
    assert(!sample.has_gesture && !sample.has_touch && !sample.has_airwheel);
    assert(sample.has_position && sample.x == 1U && sample.y == 2U && sample.z == 3U);

    uint8_t truncated[sizeof(position_only)];
    memcpy(truncated, position_only, sizeof(truncated));
    truncated[0] = 13U;
    assert(mgc3130_parse_sensor_frame(truncated,
                                      sizeof(truncated),
                                      &sample) == ESP_ERR_INVALID_SIZE);
    truncated[0] = sizeof(truncated);
    truncated[3] = 0x15U;
    assert(mgc3130_parse_sensor_frame(truncated,
                                      sizeof(truncated),
                                      &sample) == ESP_ERR_NOT_SUPPORTED);
}

int main(void)
{
    test_runtime_parameter();
    test_full_sensor_frame();
    test_variable_fields_and_errors();
    puts("mgc3130_test: ok");
    return 0;
}
