#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MGC3130_ADDRESS_PRIMARY 0x42U
#define MGC3130_ADDRESS_SECONDARY 0x43U
#define MGC3130_SENSOR_FRAME_SIZE 26U
#define MGC3130_RUNTIME_FRAME_SIZE 16U

#define MGC3130_DATA_DSP_STATUS (1U << 0)
#define MGC3130_DATA_GESTURE (1U << 1)
#define MGC3130_DATA_TOUCH (1U << 2)
#define MGC3130_DATA_AIRWHEEL (1U << 3)
#define MGC3130_DATA_POSITION (1U << 4)
#define MGC3130_DATA_NOISE_POWER (1U << 5)

#define MGC3130_SYSTEM_POSITION_VALID (1U << 0)
#define MGC3130_SYSTEM_AIRWHEEL_VALID (1U << 1)
#define MGC3130_SYSTEM_RAW_DATA_VALID (1U << 2)
#define MGC3130_SYSTEM_NOISE_POWER_VALID (1U << 3)
#define MGC3130_SYSTEM_ENVIRONMENTAL_NOISE (1U << 4)
#define MGC3130_SYSTEM_CLIPPING (1U << 5)
#define MGC3130_SYSTEM_DSP_RUNNING (1U << 7)

#define MGC3130_GESTURE_HAND_PRESENT (1UL << 27)
#define MGC3130_GESTURE_HAND_HELD (1UL << 28)
#define MGC3130_GESTURE_HAND_INSIDE (1UL << 29)
#define MGC3130_GESTURE_IN_PROGRESS (1UL << 31)

typedef struct {
    uint8_t sequence;
    uint8_t timestamp;
    uint8_t system_info;
    uint16_t data_mask;
    bool has_dsp_status;
    uint16_t dsp_status;
    bool has_gesture;
    uint32_t gesture_info;
    bool has_touch;
    uint32_t touch_info;
    bool has_airwheel;
    uint8_t airwheel;
    bool has_position;
    uint16_t x;
    uint16_t y;
    uint16_t z;
} mgc3130_sample_t;

esp_err_t mgc3130_parse_sensor_frame(const uint8_t *frame,
                                     size_t frame_len,
                                     mgc3130_sample_t *sample);
esp_err_t mgc3130_build_runtime_parameter(uint16_t parameter,
                                          uint32_t value,
                                          uint32_t mask,
                                          uint8_t sequence,
                                          uint8_t frame[MGC3130_RUNTIME_FRAME_SIZE]);
