#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
/* Deterministic digest fixture: tests transport/storage ownership and errors;
 * production SHA-256 is covered separately by the crypto tests. */
typedef struct { uint32_t value; } solar_os_crypto_sha256_t;
void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *ctx);
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *ctx);
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *ctx);
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *ctx, const void *data, size_t size);
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *ctx, uint8_t out[32]);
bool solar_os_crypto_sha256_hex_is_valid(const char *hex);
bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[32], const char *hex);
