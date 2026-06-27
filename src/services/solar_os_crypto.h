#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_CRYPTO_SHA256_LEN 32
#define SOLAR_OS_CRYPTO_SHA256_HEX_LEN 65
#define SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX 96

typedef struct {
    mbedtls_sha256_context ctx;
    bool started;
} solar_os_crypto_sha256_t;

typedef struct {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool seeded;
} solar_os_crypto_rng_t;

void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *ctx);
void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *ctx);
esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *ctx);
esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *ctx,
                                        const void *data,
                                        size_t len);
esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *ctx,
                                        uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN]);
esp_err_t solar_os_crypto_sha256_once(const void *data,
                                      size_t len,
                                      uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN]);

bool solar_os_crypto_sha256_hex_is_valid(const char *hex);
esp_err_t solar_os_crypto_bytes_to_hex(const uint8_t *bytes,
                                       size_t bytes_len,
                                       char *hex,
                                       size_t hex_len);
esp_err_t solar_os_crypto_hex_to_bytes(const char *hex,
                                       uint8_t *bytes,
                                       size_t bytes_len);
bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN],
                                        const char *hex);
esp_err_t solar_os_crypto_base64_decode(const char *text,
                                        uint8_t *out,
                                        size_t out_len,
                                        size_t *actual_len);
esp_err_t solar_os_crypto_ecdsa_p256_sha256_verify_pem(const char *public_key_pem,
                                                       const void *data,
                                                       size_t data_len,
                                                       const uint8_t *signature_der,
                                                       size_t signature_len);

esp_err_t solar_os_crypto_random(uint8_t *out, size_t len);
esp_err_t solar_os_crypto_rng_init(solar_os_crypto_rng_t *rng, const char *personalization);
void solar_os_crypto_rng_free(solar_os_crypto_rng_t *rng);
int solar_os_crypto_rng_mbedtls(void *rng, unsigned char *out, size_t len);

#ifdef __cplusplus
}
#endif
