#include "solar_os_crypto.h"

#include <ctype.h>
#include <string.h>

#include "esp_random.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"

static int crypto_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

void solar_os_crypto_sha256_init(solar_os_crypto_sha256_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    mbedtls_sha256_init(&ctx->ctx);
}

void solar_os_crypto_sha256_free(solar_os_crypto_sha256_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    mbedtls_sha256_free(&ctx->ctx);
    memset(ctx, 0, sizeof(*ctx));
}

esp_err_t solar_os_crypto_sha256_start(solar_os_crypto_sha256_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const int ret = mbedtls_sha256_starts(&ctx->ctx, 0);
    if (ret != 0) {
        ctx->started = false;
        return ESP_FAIL;
    }
    ctx->started = true;
    return ESP_OK;
}

esp_err_t solar_os_crypto_sha256_update(solar_os_crypto_sha256_t *ctx,
                                        const void *data,
                                        size_t len)
{
    if (ctx == NULL || (data == NULL && len > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len == 0) {
        return ESP_OK;
    }

    return mbedtls_sha256_update(&ctx->ctx, (const unsigned char *)data, len) == 0 ?
        ESP_OK :
        ESP_FAIL;
}

esp_err_t solar_os_crypto_sha256_finish(solar_os_crypto_sha256_t *ctx,
                                        uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN])
{
    if (ctx == NULL || digest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ctx->started) {
        return ESP_ERR_INVALID_STATE;
    }

    const int ret = mbedtls_sha256_finish(&ctx->ctx, digest);
    ctx->started = false;
    return ret == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t solar_os_crypto_sha256_once(const void *data,
                                      size_t len,
                                      uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN])
{
    if ((data == NULL && len > 0) || digest == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    solar_os_crypto_sha256_t ctx;
    solar_os_crypto_sha256_init(&ctx);
    esp_err_t err = solar_os_crypto_sha256_start(&ctx);
    if (err == ESP_OK) {
        err = solar_os_crypto_sha256_update(&ctx, data, len);
    }
    if (err == ESP_OK) {
        err = solar_os_crypto_sha256_finish(&ctx, digest);
    }
    solar_os_crypto_sha256_free(&ctx);
    return err;
}

bool solar_os_crypto_sha256_hex_is_valid(const char *hex)
{
    if (hex == NULL || strlen(hex) != SOLAR_OS_CRYPTO_SHA256_LEN * 2U) {
        return false;
    }

    for (const unsigned char *p = (const unsigned char *)hex; *p != '\0'; p++) {
        if (!isxdigit(*p)) {
            return false;
        }
    }
    return true;
}

esp_err_t solar_os_crypto_bytes_to_hex(const uint8_t *bytes,
                                       size_t bytes_len,
                                       char *hex,
                                       size_t hex_len)
{
    static const char digits[] = "0123456789abcdef";
    if (bytes == NULL || hex == NULL || hex_len < bytes_len * 2U + 1U) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < bytes_len; i++) {
        hex[i * 2U] = digits[bytes[i] >> 4];
        hex[i * 2U + 1U] = digits[bytes[i] & 0x0fU];
    }
    hex[bytes_len * 2U] = '\0';
    return ESP_OK;
}

esp_err_t solar_os_crypto_hex_to_bytes(const char *hex,
                                       uint8_t *bytes,
                                       size_t bytes_len)
{
    if (hex == NULL || bytes == NULL || strlen(hex) != bytes_len * 2U) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < bytes_len; i++) {
        const int hi = crypto_hex_value(hex[i * 2U]);
        const int lo = crypto_hex_value(hex[i * 2U + 1U]);
        if (hi < 0 || lo < 0) {
            return ESP_ERR_INVALID_ARG;
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

bool solar_os_crypto_sha256_matches_hex(const uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN],
                                        const char *hex)
{
    uint8_t expected[SOLAR_OS_CRYPTO_SHA256_LEN];
    if (digest == NULL ||
        solar_os_crypto_hex_to_bytes(hex, expected, sizeof(expected)) != ESP_OK) {
        return false;
    }
    return memcmp(digest, expected, sizeof(expected)) == 0;
}

esp_err_t solar_os_crypto_base64_decode(const char *text,
                                        uint8_t *out,
                                        size_t out_len,
                                        size_t *actual_len)
{
    if (text == NULL || out == NULL || out_len == 0 || actual_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t decoded_len = 0;
    const int ret = mbedtls_base64_decode(out,
                                          out_len,
                                          &decoded_len,
                                          (const unsigned char *)text,
                                          strlen(text));
    if (ret == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ret != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *actual_len = decoded_len;
    return ESP_OK;
}

esp_err_t solar_os_crypto_ecdsa_p256_sha256_verify_pem(const char *public_key_pem,
                                                       const void *data,
                                                       size_t data_len,
                                                       const uint8_t *signature_der,
                                                       size_t signature_len)
{
    if (public_key_pem == NULL ||
        (data == NULL && data_len > 0) ||
        signature_der == NULL ||
        signature_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
    esp_err_t err = solar_os_crypto_sha256_once(data, data_len, digest);
    if (err != ESP_OK) {
        return err;
    }

    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    const int parse_ret = mbedtls_pk_parse_public_key(&key,
                                                      (const unsigned char *)public_key_pem,
                                                      strlen(public_key_pem) + 1U);
    if (parse_ret != 0) {
        mbedtls_pk_free(&key);
        return ESP_ERR_INVALID_ARG;
    }

    if (!mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA) || mbedtls_pk_get_bitlen(&key) != 256) {
        mbedtls_pk_free(&key);
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int verify_ret = mbedtls_pk_verify(&key,
                                             MBEDTLS_MD_SHA256,
                                             digest,
                                             sizeof(digest),
                                             signature_der,
                                             signature_len);
    mbedtls_pk_free(&key);
    return verify_ret == 0 ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t solar_os_crypto_random(uint8_t *out, size_t len)
{
    if (out == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > 0) {
        esp_fill_random(out, len);
    }
    return ESP_OK;
}

esp_err_t solar_os_crypto_rng_init(solar_os_crypto_rng_t *rng, const char *personalization)
{
    if (rng == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(rng, 0, sizeof(*rng));
    mbedtls_entropy_init(&rng->entropy);
    mbedtls_ctr_drbg_init(&rng->ctr_drbg);

    if (personalization == NULL) {
        personalization = "SolarOS";
    }

    const int ret = mbedtls_ctr_drbg_seed(&rng->ctr_drbg,
                                          mbedtls_entropy_func,
                                          &rng->entropy,
                                          (const unsigned char *)personalization,
                                          strlen(personalization));
    if (ret != 0) {
        solar_os_crypto_rng_free(rng);
        return ESP_FAIL;
    }
    rng->seeded = true;
    return ESP_OK;
}

void solar_os_crypto_rng_free(solar_os_crypto_rng_t *rng)
{
    if (rng == NULL) {
        return;
    }
    mbedtls_ctr_drbg_free(&rng->ctr_drbg);
    mbedtls_entropy_free(&rng->entropy);
    memset(rng, 0, sizeof(*rng));
}

int solar_os_crypto_rng_mbedtls(void *rng, unsigned char *out, size_t len)
{
    solar_os_crypto_rng_t *crypto_rng = (solar_os_crypto_rng_t *)rng;
    if (crypto_rng == NULL || out == NULL || !crypto_rng->seeded) {
        return -1;
    }
    return mbedtls_ctr_drbg_random(&crypto_rng->ctr_drbg, out, len);
}
