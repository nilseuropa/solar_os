#pragma once

#include <stddef.h>
#include <stdint.h>

/* Packed data starts with the little-endian length of its zlib stream. */
uint8_t *solar_os_flash_stub_unpack(const uint8_t *packed, size_t size);
void solar_os_flash_stub_release(uint8_t *data);
