#pragma once
/* Host crypto adapter uses OpenSSL; firmware uses the real mbedTLS service. */
typedef struct { void *pointer; } mbedtls_sha256_context;
