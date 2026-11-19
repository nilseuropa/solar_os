#pragma once
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_PROCESSOR_PROVIDER_ABI 1U
#define SOLAR_OS_PROCESSOR_PROVIDER_JSON_MAX 65536U
#define SOLAR_OS_PROCESSOR_PROVIDER_CAPTURE (1U << 0)
#define SOLAR_OS_PROCESSOR_PROVIDER_MODEL_SELECT (1U << 1)
#define SOLAR_OS_PROCESSOR_PROVIDER_MODEL_UPLOAD (1U << 2)
#define SOLAR_OS_PROCESSOR_UPLOAD_REPLACE (1U << 0)
typedef void (*solar_os_processor_progress_fn)(const char *stage, uint32_t bytes, uint32_t total, void *user);

typedef const void *(*solar_os_processor_service_fn)(const char *, uint32_t, uint32_t);
typedef struct {
    const uint8_t *data;
    size_t length;
    uint16_t width, height;
    uint64_t timestamp_us;
} solar_os_processor_frame_v1_t;

/* Installed as processor-<kind>. All results use native service error codes.
 * host service tables remain valid for the client lifetime. JSON uses host
 * memory and is independent of the provider lease. capture returns a borrowed
 * encoded frame and matching result from one acquisition; release_frame must
 * follow every capture, including failure. close joins work and releases all
 * resources before returning OK; failed close leaves a live, retryable client.
 * Calls on a client must be serialized. This ABI describes camera-bound
 * processors; it does not imply support for arbitrary uploaded images. */
typedef struct {
    uint32_t abi_version, struct_size;
    const char *kind;
    uint32_t capabilities;
    int (*query)(solar_os_processor_service_fn services, const char *source,
        const char *query, uint32_t timeout_ms, char **json);
    int (*open)(solar_os_processor_service_fn services, const char *source,
        uint32_t model, uint32_t timeout_ms, solar_os_native_cancel_fn cancel,
        void *user, void **client);
    int (*capture)(void *client, solar_os_processor_frame_v1_t *frame, char **json);
    void (*release_frame)(void *client);
    int (*close)(void *client);
    /* Optional model management. Upload validates a storage bundle and programs
     * an explicit inactive device slot; it must restore the previous selection
     * before returning. Provider policy owns compatibility and programming.
     * Calls are synchronous, cancellable, and hold the library lease; a job
     * can run upload without retaining shell/session pointers. JSON is owned.
     * Protocol timeout applies to each transaction, not the complete upload. */
    int (*select)(solar_os_processor_service_fn services, const char *source,
        uint32_t slot, uint32_t timeout_ms, solar_os_native_cancel_fn cancel, void *user, char **json);
    int (*upload)(solar_os_processor_service_fn services, const char *source,
        const char *bundle, uint32_t slot, uint32_t flags, uint32_t timeout_ms,
        solar_os_native_cancel_fn cancel, solar_os_processor_progress_fn progress, void *user, char **json);
} solar_os_processor_provider_api_v1_t;

#define SOLAR_OS_PROCESSOR_PROVIDER_BASE_SIZE offsetof(solar_os_processor_provider_api_v1_t, select)
