/*
 * Copyright (c) 2026 Chad Attermann
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 */

#pragma once

#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <chrono>
#include <sys/time.h>
#endif

#include <cstring>

namespace microStore {

/* ---------------- TIME HELPER ---------------- */

#ifdef ARDUINO
// millis() is a reliable Arduino built-in on both ESP32 and nRF52840.
// std::chrono::steady_clock is intentionally avoided: the Adafruit nRF52
// Arduino core lacks the _gettimeofday_r backend and causes linker errors.
inline static uint32_t millis() {
	return (uint32_t)::millis();
}
// CBA static inline variables in < C++17 so must use workaround instead
// set time offset for devices without a real-time clock
//inline static uint32_t time_offset = 0;
//inline static void set_time_offset(uint32_t offset) {
//	time_offset = offset;
//}
inline uint32_t& time_offset() {
    static uint32_t g_time_offet = 0;
    return g_time_offet;
}
inline void set_time_offset(uint32_t offset) {
	time_offset() = offset;
}
// return current time in seconds since startup
inline static uint32_t time() {
	// handle roll-over of 32-bit millis (approx. 49 days)
	static uint32_t low32, high32;
	uint32_t new_low32 = ::millis();
	if (new_low32 < low32) high32++;
	low32 = new_low32;
	return (uint32_t)(((uint64_t)high32 << 32 | low32)/1000) + time_offset();
}
#else
inline static uint32_t millis() {
	using namespace std::chrono;
	return (uint32_t)duration_cast<milliseconds>(
		steady_clock::now().time_since_epoch()).count();
}
// return current time in seconds since 00:00:00, January 1, 1970 (Unix Epoch)
inline static uint32_t time() {
	timeval time;
	::gettimeofday(&time, NULL);
	return (uint32_t)(((uint64_t)(time.tv_sec * 1000) + (uint64_t)(time.tv_usec / 1000))/1000);
}
#endif

/* ---------------- CRC ---------------- */

static uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len)
{
	if (data == nullptr) return 0;
	crc = ~crc;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for(int j = 0; j<8; j++)
			crc = (crc>>1) ^ (0xEDB88320 & -(crc&1));
	}
	return ~crc;
}
/*
static uint32_t crc32(uint32_t crc, const uint8_t* buffer, size_t len) {
	const unsigned char *data = (const unsigned char *)buffer;
	if (data == NULL)
		return 0;
	crc ^= 0xffffffff;
	while (len--) {
		crc ^= *data++;
		for (unsigned k = 0; k < 8; k++)
			crc = crc & 1 ? (crc >> 1) ^ 0xedb88320 : crc >> 1;
	}
	return crc ^ 0xffffffff;
}
*/
inline static uint32_t crc32(uint32_t crc, uint8_t byte) { return crc32(crc, &byte, sizeof(byte)); }
inline static uint32_t crc32(uint32_t crc, const char* str) { return crc32(crc, (const uint8_t*)str, strlen(str)); }

}
