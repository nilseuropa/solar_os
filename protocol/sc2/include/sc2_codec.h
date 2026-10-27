/*
 * SC2 portable core: base64, hex and padding buckets.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_CODEC_H
#define SC2_CODEC_H

#include "sc2_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard base64 (RFC 4648 §4) with '=' padding and no line breaks. */
#define SC2_B64_ENCODED_LEN(n) ((((n) + 2u) / 3u) * 4u)

/* Writes the encoding plus a NUL terminator; cap must be >= encoded + 1. */
int sc2_b64_encode(char *out, size_t cap, size_t *out_len, const uint8_t *in, size_t len);
/* Strict decode: padding required, no whitespace, unused bits must be zero.
 * Decodes front to back, so out may alias in or start before it (in-place
 * decoding into the same buffer is allowed). */
int sc2_b64_decode(uint8_t *out, size_t cap, size_t *out_len, const char *in, size_t len);

/* Lowercase hex with NUL terminator; cap must be >= 2 * len + 1. */
int sc2_hex_encode(char *out, size_t cap, const uint8_t *in, size_t len);
/* Decodes exactly out_len bytes from 2 * out_len hex digits (either case). */
int sc2_hex_decode(uint8_t *out, size_t out_len, const char *in, size_t in_len);

/* Padding buckets (SC2-1): x || 0x80 || 0x00... up to 256/512/1024/2048/4096/8192. */
#define SC2_PAD_MAX_BUCKET 8192u
#define SC2_PAD_MAX_INPUT (SC2_PAD_MAX_BUCKET - 1u)

/* Bucket size for an input of len bytes, or 0 if len is too large. */
size_t sc2_pad_bucket(size_t len);
/* out may equal in. */
int sc2_pad(uint8_t *out, size_t cap, size_t *out_len, const uint8_t *in, size_t len);
/* Returns the unpadded length in *out_len; buf must be a whole bucket. */
int sc2_unpad(const uint8_t *buf, size_t len, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
