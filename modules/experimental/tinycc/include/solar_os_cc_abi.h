#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_CC_LIBRARY "tinycc"
#define SOLAR_OS_CC_ABI 1U
#define SOLAR_OS_CC_SOURCE_MAX 65536U
#define SOLAR_OS_CC_OUTPUT_MAX (2U * 1024U * 1024U)
#define SOLAR_OS_CC_HEAP_MAX (2U * 1024U * 1024U)
#define SOLAR_OS_CC_SOURCE_FILES_MAX 8U

typedef struct {
    const char *name;
    const char *text;
    uint32_t bytes;
} solar_os_cc_header_v1_t;

/* Optional file adapter, called on the compiler worker. With buffer=NULL,
 * return the size through bytes; otherwise read the complete file into the
 * bounded buffer. Return NOT_FOUND to continue include-path searching. */
typedef int (*solar_os_cc_read_header_fn)(void *user, const char *path,
    void *buffer, uint32_t capacity, uint32_t *bytes);

typedef struct {
    const char *name;
    const char *text;
    uint32_t bytes;
} solar_os_cc_source_v1_t;

typedef struct {
    uint32_t struct_size;
    const char *source;
    uint32_t source_bytes;
    const solar_os_cc_header_v1_t *headers;
    uint32_t header_count;
    const char *entry; /* NULL selects main. */
    uint32_t heap_limit; /* 0 selects 2 MiB; otherwise 64 KiB..2 MiB. */
    uint32_t timeout_ms; /* 0 selects 2000 ms; otherwise 1..20000 ms. */
    solar_os_native_cancel_fn cancel;
    void *cancel_user;
    /* Append-only extension. Older request prefixes remain supported. */
    const char *source_name;
    const char *const *include_paths;
    uint32_t include_count;
    solar_os_cc_read_header_fn read_header;
    void *header_user;
    /* Optional translation units. When count>0 these replace source/source_name.
     * Compile separately in one request; the total source limit remains 64 KiB. */
    const solar_os_cc_source_v1_t *sources;
    uint32_t source_count;
} solar_os_cc_request_v1_t;

#define SOLAR_OS_CC_REQUEST_BASE_SIZE offsetof(solar_os_cc_request_v1_t, source_name)
#define SOLAR_OS_CC_REQUEST_FILE_SIZE offsetof(solar_os_cc_request_v1_t, sources)

typedef struct {
    uint32_t struct_size;
    uint32_t output_bytes; /* Zero on failure. */
    uint32_t peak_heap_bytes;
    uint32_t elapsed_ms;
} solar_os_cc_result_v1_t;

/* One compile at a time. Inputs remain immutable and cancellation code valid until the
 * call returns; cancel runs on a joined compiler worker. Output and diagnostics
 * are caller-owned and remain valid after releasing the library lease.
 * Headers form a read-only virtual filesystem; optional file access is routed
 * through the caller's bounded adapter. It frees request allocations and joins
 * the worker before return.
 * A library lease must protect this call and its callbacks throughout. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*compile)(const solar_os_cc_request_v1_t *request,
                   void *output, uint32_t capacity,
                   solar_os_cc_result_v1_t *result,
                   char *diagnostics, uint32_t diagnostics_size);
    uint32_t request_size; /* Append-only capability: largest supported request. */
    /* Read-only built-in SDK, borrowed while the provider lease is held.
     * These entries need no compilation worker and remain immutable. */
    const char *sdk_version;
    uint32_t sdk_count;
    const solar_os_cc_header_v1_t *sdk_headers;
} solar_os_cc_api_v1_t;

#define SOLAR_OS_CC_API_BASE_SIZE offsetof(solar_os_cc_api_v1_t, sdk_version)
