/*
 * SC2 portable core: safety numbers and fingerprints.
 *
 *   h = SHA-256("SC2-SAFETY\x01" || min(IK_a, IK_b) || max(IK_a, IK_b))
 *   short: 6 groups, u40be(h[5i..5i+5)) mod 10^4  -> "dddd dddd dddd dddd dddd dddd"
 *   words: 8 x 11-bit big-endian groups of h[0..11) -> BIP-39 English
 *   full : m = HKDF-Expand(h, "SC2-SAFETY-FULL\x01", 60);
 *          12 groups, u40be(m[5i..5i+5)) mod 10^5 -> "ddddd ddddd ..."
 *   own fingerprint: short form of SHA-256(IK)
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_SAFETY_H
#define SC2_SAFETY_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_SAFETY_SHORT_LEN 29u  /* without NUL */
#define SC2_SAFETY_FULL_LEN 71u   /* without NUL */
#define SC2_SAFETY_WORDS 8u
#define SC2_SAFETY_WORDS_MAX 72u  /* 8 words of <= 8 letters + 7 separators, without NUL */
#define SC2_WORDLIST_SIZE 2048u

int sc2_safety_hash(uint8_t h[32], const uint8_t ik_a[SC2_KEY_LEN], const uint8_t ik_b[SC2_KEY_LEN]);

int sc2_safety_short(char out[SC2_SAFETY_SHORT_LEN + 1], const uint8_t h[32]);
int sc2_safety_full(char out[SC2_SAFETY_FULL_LEN + 1], const uint8_t h[32]);
/* Word indices (11 bits each). */
int sc2_safety_word_indices(uint16_t idx[SC2_SAFETY_WORDS], const uint8_t h[32]);
/* Words joined by sep (e.g. '-' or ' '). */
int sc2_safety_words(char *out, size_t cap, const uint8_t h[32], char sep);

/* Own fingerprint: short form of SHA-256(IK). */
int sc2_fingerprint_short(char out[SC2_SAFETY_SHORT_LEN + 1], const uint8_t ik[SC2_KEY_LEN]);

/* BIP-39 English word list. Returns NULL for idx >= 2048. */
const char *sc2_wordlist_en(uint16_t idx);

#ifdef __cplusplus
}
#endif

#endif
