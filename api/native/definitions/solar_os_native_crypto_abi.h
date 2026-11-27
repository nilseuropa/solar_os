#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_CRYPTO_SERVICE "crypto"
#define SOLAR_OS_NATIVE_CRYPTO_ABI 1U

/* SHA-256 and random-generator state stays in the host. A successful hash_finish
 * ends that hash stream; start may reset it. Closing a handle frees its state.
 * No callback or input pointer is retained. Serialize operations on each token.
 * Caller buffers retain their ordinary C lifetime/size requirements. Random
 * reads split provider-sized requests internally. Close every token before exit. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*hash_create)(solar_os_native_handle_t *hash);
    int (*hash_start)(solar_os_native_handle_t hash);
    int (*hash_update)(solar_os_native_handle_t hash, const void *bytes, size_t size);
    int (*hash_finish)(solar_os_native_handle_t hash, uint8_t digest[32]);
    int (*hash_once)(const void *bytes, size_t size, uint8_t digest[32]);
    int (*close)(solar_os_native_handle_t handle);
    int (*random)(uint8_t *bytes, size_t size);
    int (*random_create)(const char *personalization, solar_os_native_handle_t *rng);
    int (*random_read)(solar_os_native_handle_t rng, uint8_t *bytes, size_t size);
    bool (*hash_hex_valid)(const char *hex);
    int (*bytes_to_hex)(const uint8_t *bytes, size_t size, char *hex, size_t capacity);
    int (*hex_to_bytes)(const char *hex, uint8_t *bytes, size_t size);
    bool (*hash_matches_hex)(const uint8_t digest[32], const char *hex);
    int (*base64_decode)(const char *text, uint8_t *bytes, size_t capacity, size_t *size);
    int (*verify_p256_sha256)(const char *pem, const void *data, size_t size,
        const uint8_t *signature, size_t signature_size);
    /* Overwrite secret material before the caller releases its buffer. */
    void (*wipe)(void *bytes, size_t size);
} solar_os_native_crypto_api_v1_t;
