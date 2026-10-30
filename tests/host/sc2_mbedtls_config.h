/*
 * Minimal mbedTLS configuration for the SC2 host backend test: only what
 * protocol/sc2/src/sc2_hash.c needs with SC2_USE_MBEDTLS=1 (SHA-256 + the
 * generic MD layer for HMAC). Used with the ESP-IDF copy of mbedTLS 3.6.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_MBEDTLS_CONFIG_H
#define SC2_MBEDTLS_CONFIG_H

#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C

#endif
