#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Convert one CardKB wire value to a SolarOS character or logical key. */
bool solar_os_cardkb_decode(uint8_t value, uint8_t *key);
