/* Experimental independent LZ4-block decoder. Not part of the DLP contract.
 * Block syntax: https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 * Output is bounded by the caller's negotiated 1024-byte scratch buffer.
 */
#ifndef SOLAR_OS_DLP_LOSSLESS_TRIAL_DECODER_H
#define SOLAR_OS_DLP_LOSSLESS_TRIAL_DECODER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool solar_os_dlp_trial_lz4_decode(uint8_t *out, size_t expected,
                                        const uint8_t *in, size_t size) {
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
    memcpy(out + used, in + ip, literals);
    ip += literals;
    used += literals;
    if (ip == size) return used == expected;
    if (size - ip < 2) return false;
    size_t offset = in[ip] | (size_t)in[ip + 1] << 8;
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
    for (size_t k = 0; k < match; k++) {
      out[used] = out[used - offset];
      used++;
    }
  }
  return false; /* A block ends with a literal sequence. */
}
#endif
