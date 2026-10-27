/* SPDX-License-Identifier: MIT
 * Independent LZ4 blocks, bounded to SDLP_RLE_MAX_DECODED. No heap or dictionary.
 * Format: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 */
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
  size_t ip = 0, used = 0;
  if (!out || !in || !expected || expected > SDLP_RLE_MAX_DECODED || !size) return false;
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
    size_t offset = sdlp_get_u16(in + ip); ip += 2;
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
