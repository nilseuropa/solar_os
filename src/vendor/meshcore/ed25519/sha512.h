#ifndef SHA512_H
#define SHA512_H

#include <stddef.h>

#include "fixedint.h"

#ifdef ESP_PLATFORM
#include "mbedtls/sha512.h"
#endif

/* Reuse the firmware's software SHA-512 where it is already linked.
 * Keep the allocation-free reference backend on hardware/DMA builds. */
#if defined(ESP_PLATFORM) && defined(MBEDTLS_SHA512_C) && !defined(MBEDTLS_SHA512_ALT)
#define SOLAR_OS_ED25519_SHARED_SHA512 1
typedef mbedtls_sha512_context sha512_context;
#else
/* state */
typedef struct sha512_context_ {
    uint64_t  length, state[8];
    size_t curlen;
    unsigned char buf[128];
} sha512_context;
#endif


int sha512_init(sha512_context * md);
int sha512_final(sha512_context * md, unsigned char *out);
int sha512_update(sha512_context * md, const unsigned char *in, size_t inlen);
int sha512(const unsigned char *message, size_t message_len, unsigned char *out);

#endif
