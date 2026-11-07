#pragma once

// microReticulum seeds and services rweather's software RNG from Reticulum.cpp.
// SolarOS draws every random byte from the ESP32 hardware RNG in
// microReticulum/Cryptography/Random.h, so there is nothing to seed or service.
struct SolarOsRng {
	void begin(const char*) {}
	void loop() {}
};

inline SolarOsRng RNG;
