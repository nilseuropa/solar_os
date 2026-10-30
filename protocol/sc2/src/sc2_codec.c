/*
 * SC2 portable core: base64, hex and padding buckets.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_codec.h"

#include <string.h>

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int sc2_b64_encode(char *out, size_t cap, size_t *out_len, const uint8_t *in, size_t len)
{
    if (out == NULL || (in == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    if (len > (SIZE_MAX / 4u) * 3u - 3u) {
        return SC2_ERR_BOUNDS;
    }
    size_t need = SC2_B64_ENCODED_LEN(len);
    if (cap < need + 1u) {
        return SC2_ERR_BOUNDS;
    }
    size_t o = 0;
    size_t i = 0;
    while (i + 3u <= len) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = B64[(v >> 6) & 63u];
        out[o++] = B64[v & 63u];
        i += 3u;
    }
    if (len - i == 1u) {
        uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2u) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[o++] = B64[(v >> 18) & 63u];
        out[o++] = B64[(v >> 12) & 63u];
        out[o++] = B64[(v >> 6) & 63u];
        out[o++] = '=';
    }
    out[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return SC2_OK;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int sc2_b64_decode(uint8_t *out, size_t cap, size_t *out_len, const char *in, size_t len)
{
    if ((in == NULL && len != 0) || (out == NULL && cap != 0)) {
        return SC2_ERR_ARG;
    }
    if (len % 4u != 0) {
        return SC2_ERR_FORMAT;
    }
    size_t pad = 0;
    if (len >= 4u) {
        if (in[len - 1] == '=') {
            pad++;
            if (in[len - 2] == '=') {
                pad++;
            }
        }
    }
    size_t need = (len / 4u) * 3u - pad;
    if (need > cap) {
        return SC2_ERR_BOUNDS;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 4u) {
        int v[4];
        size_t quad_pad = (i + 4u == len) ? pad : 0;
        for (size_t k = 0; k < 4u; k++) {
            if (k >= 4u - quad_pad) {
                v[k] = 0;
                continue;
            }
            v[k] = b64_val(in[i + k]);
            if (v[k] < 0) {
                return SC2_ERR_FORMAT;
            }
        }
        uint32_t w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
        if (quad_pad == 2u && (w & 0xFFFFu) != 0) {
            return SC2_ERR_FORMAT; /* non-zero unused bits */
        }
        if (quad_pad == 1u && (w & 0xFFu) != 0) {
            return SC2_ERR_FORMAT;
        }
        out[o++] = (uint8_t)(w >> 16);
        if (quad_pad < 2u) {
            out[o++] = (uint8_t)(w >> 8);
        }
        if (quad_pad < 1u) {
            out[o++] = (uint8_t)w;
        }
    }
    if (out_len) {
        *out_len = o;
    }
    return SC2_OK;
}

int sc2_hex_encode(char *out, size_t cap, const uint8_t *in, size_t len)
{
    static const char HEX[] = "0123456789abcdef";
    if (out == NULL || (in == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    if (len > (SIZE_MAX - 1u) / 2u || cap < 2u * len + 1u) {
        return SC2_ERR_BOUNDS;
    }
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = HEX[in[i] >> 4];
        out[2 * i + 1] = HEX[in[i] & 15u];
    }
    out[2 * len] = '\0';
    return SC2_OK;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

int sc2_hex_decode(uint8_t *out, size_t out_len, const char *in, size_t in_len)
{
    if ((out == NULL && out_len != 0) || (in == NULL && in_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (in_len != 2u * out_len) {
        return SC2_ERR_FORMAT;
    }
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(in[2 * i]);
        int lo = hex_val(in[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return SC2_ERR_FORMAT;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return SC2_OK;
}

size_t sc2_pad_bucket(size_t len)
{
    size_t bucket = 256u;
    while (bucket <= SC2_PAD_MAX_BUCKET) {
        if (len + 1u <= bucket) {
            return bucket;
        }
        bucket *= 2u;
    }
    return 0;
}

int sc2_pad(uint8_t *out, size_t cap, size_t *out_len, const uint8_t *in, size_t len)
{
    if (out == NULL || (in == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    size_t bucket = sc2_pad_bucket(len);
    if (bucket == 0) {
        return SC2_ERR_BOUNDS;
    }
    if (cap < bucket) {
        return SC2_ERR_BOUNDS;
    }
    if (len && out != in) {
        memmove(out, in, len);
    }
    out[len] = 0x80;
    memset(out + len + 1u, 0, bucket - len - 1u);
    if (out_len) {
        *out_len = bucket;
    }
    return SC2_OK;
}

int sc2_unpad(const uint8_t *buf, size_t len, size_t *out_len)
{
    if (buf == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    if (len < 256u || len > SC2_PAD_MAX_BUCKET || (len & (len - 1u)) != 0) {
        return SC2_ERR_FORMAT;
    }
    size_t i = len;
    while (i > 0 && buf[i - 1u] == 0) {
        i--;
    }
    if (i == 0 || buf[i - 1u] != 0x80) {
        return SC2_ERR_FORMAT;
    }
    size_t n = i - 1u;
    if (sc2_pad_bucket(n) != len) {
        return SC2_ERR_FORMAT; /* not the minimal bucket */
    }
    *out_len = n;
    return SC2_OK;
}
