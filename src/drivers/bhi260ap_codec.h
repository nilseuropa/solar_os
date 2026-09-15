#pragma once

#include <stdint.h>

void bhi260ap_decode_acceleration(const uint8_t data[6], float output_m_s2[3]);
void bhi260ap_decode_angular_velocity(const uint8_t data[6],
                                      float output_rad_s[3]);
void bhi260ap_decode_quaternion(const uint8_t data[8], float output_wxyz[4]);
uint64_t bhi260ap_timestamp_us(uint64_t sensor_timestamp);
