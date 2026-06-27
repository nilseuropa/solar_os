#include "solar_os_crypto.h"

#include <ctype.h>
#include <string.h>

#include "esp_random.h"

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
