#pragma once
#include <stddef.h>
int mbedtls_base64_decode(unsigned char *dst, size_t capacity, size_t *length,
                          const unsigned char *source, size_t size);
