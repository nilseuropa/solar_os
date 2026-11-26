#pragma once

#include <openssl/evp.h>

static inline int mbedtls_md5(const unsigned char *data, size_t length, unsigned char output[16])
{
    unsigned int size = 0;
    return EVP_Digest(data, length, output, &size, EVP_md5(), NULL) && size == 16 ? 0 : -1;
}
