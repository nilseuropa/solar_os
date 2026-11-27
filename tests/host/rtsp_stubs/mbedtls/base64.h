#pragma once

#include <limits.h>
#include <openssl/evp.h>

static inline int mbedtls_base64_encode(unsigned char *out, size_t capacity, size_t *written,
    const unsigned char *in, size_t length)
{
    if (length > INT_MAX / 2 || 4 * ((length + 2) / 3) >= capacity) return -1;
    *written = (size_t)EVP_EncodeBlock(out, in, (int)length);
    return 0;
}
