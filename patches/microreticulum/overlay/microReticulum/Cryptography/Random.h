#pragma once

#include "../Bytes.h"

#include <esp_random.h>

#include <stdint.h>

// esp_fill_random() is a true random source only while Wi-Fi or Bluetooth is
// on, or inside bootloader_random_enable(); otherwise it is a pseudo-random
// generator. Keys must be made under one of those (see README.solaros.md).

namespace RNS { namespace Cryptography {

	inline const Bytes random(size_t length) {
		Bytes rand;
		esp_fill_random(rand.writable(length), length);
		return rand;
	}

	inline uint32_t randomnum() {
		return esp_random();
	}

	inline uint32_t randomnum(uint32_t max) {
		return max == 0 ? 0 : randomnum() % max;
	}

	inline float random() {
		return (float)(randomnum() / (float)0xffffffff);
	}

} }
