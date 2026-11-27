/* SPDX-License-Identifier: MIT
 * Independent LZ4 blocks, bounded to SDLP_RLE_MAX_DECODED. No heap or dictionary.
 * Format: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 */
#include "sdlp_lz4_block.h"

static bool lz4_length(uint8_t *out, size_t cap, size_t *at, size_t length) {
  while (length >= 255) {
    if (*at == cap) return false;
    out[(*at)++] = 255;
    length -= 255;
  }
  if (*at == cap) return false;
  out[(*at)++] = (uint8_t)length;
  return true;
}

size_t sdlp_lz4_encode(uint8_t *out, size_t cap, const uint8_t *in, size_t size) {
  if (!out || !in || !size || size > SDLP_RLE_MAX_DECODED || !cap) return 0;
  uint16_t table[256] = {0};
  size_t at = 0, anchor = 0, ip = 0;
  while (ip + 12 <= size) {
    uint32_t word = sdlp_get_u32(in + ip);
    unsigned hash = (word * UINT32_C(2654435761)) >> 24;
    unsigned entry = table[hash];
    table[hash] = (uint16_t)(ip + 1);
    if (!entry || memcmp(in + entry - 1, in + ip, 4)) { ip++; continue; }
    size_t previous = entry - 1, match = 4, literals = ip - anchor;
    while (ip + match < size - 5 && in[previous + match] == in[ip + match]) match++;
    if (at == cap) return 0;
    out[at++] = (uint8_t)((literals < 15 ? literals : 15) << 4 |
                         (match - 4 < 15 ? match - 4 : 15));
    if (literals >= 15 && !lz4_length(out, cap, &at, literals - 15)) return 0;
    if (literals > cap - at) return 0;
    memcpy(out + at, in + anchor, literals); at += literals;
    if (cap - at < 2) return 0;
    sdlp_put_u16(out + at, (uint16_t)(ip - previous)); at += 2;
    if (match >= 19 && !lz4_length(out, cap, &at, match - 19)) return 0;
    ip += match; anchor = ip;
  }
  size_t literals = size - anchor;
  if (at == cap) return 0;
  out[at++] = (uint8_t)((literals < 15 ? literals : 15) << 4);
  if (literals >= 15 && !lz4_length(out, cap, &at, literals - 15)) return 0;
  if (literals > cap - at) return 0;
  memcpy(out + at, in + anchor, literals);
  return at + literals;
}

bool sdlp_lz4_decode(uint8_t *out, size_t expected, const uint8_t *in, size_t size) {
  return expected <= SDLP_RLE_MAX_DECODED && solar_os_lz4_decode_block(out, expected, in, size);
}
