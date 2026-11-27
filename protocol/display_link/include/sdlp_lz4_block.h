/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Decode one independent LZ4 block into an exactly sized caller buffer. */
static inline bool solar_os_lz4_decode_block(uint8_t *out, size_t expected,
                                            const uint8_t *in, size_t size)
{
  size_t ip = 0, used = 0;
  if (!out || !in || !expected || !size) return false;
  while (ip < size) {
    unsigned token = in[ip++];
    size_t literals = token >> 4;
    if (literals == 15) {
      unsigned extra;
      do {
        if (ip == size) return false;
        extra = in[ip++];
        if (literals > expected || extra > expected - literals) return false;
        literals += extra;
      } while (extra == 255);
    }
    if (literals > size - ip || literals > expected - used) return false;
    memcpy(out + used, in + ip, literals); ip += literals; used += literals;
    if (ip == size) return used == expected;
    if (size - ip < 2) return false;
    size_t offset = (size_t)in[ip] | (size_t)in[ip + 1] << 8;
    ip += 2;
    if (!offset || offset > used) return false;
    size_t match = (token & 15) + 4;
    if ((token & 15) == 15) {
      unsigned extra;
      do {
        if (ip == size) return false;
        extra = in[ip++];
        if (match > expected || extra > expected - match) return false;
        match += extra;
      } while (extra == 255);
    }
    if (match > expected - used) return false;
    for (size_t k = 0; k < match; k++, used++) out[used] = out[used - offset];
  }
  return false;
}
