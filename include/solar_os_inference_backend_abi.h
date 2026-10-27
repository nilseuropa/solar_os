#pragma once
#include "solar_os_inference_types_v1.h"
#include "solar_os_native_service_abi.h"

#define SOLAR_OS_INFERENCE_BACKEND_ABI 1U
#define SOLAR_OS_INFERENCE_BACKEND_LIBRARY "espdl"

/* Provider operations are serialized by the caller. No C++ objects or exceptions
 * cross this boundary. Keep a library lease through close of every backend.
 * load_buffer copies validated model bytes; the caller may release its input
 * after return. Pass zero-initialized info/result. Their buffers use the host
 * memory service: free every input/output exponent vector and output data buffer
 * on success or failure, including entries beyond count after a partial failure.
 * These copied buffers may outlive the backend and its library lease.
 * Cancellation is synchronous and cooperative between layers. close must join
 * all workers owned by a backend and return OK for a valid backend. Firmware
 * providers cannot defer destruction or retain its lease after close. */
typedef struct {
    uint32_t abi_version, struct_size;
    const char *version;
    int (*load_buffer)(const uint8_t *data, size_t bytes,
        solar_os_inference_cancel_fn cancel, void *user, void **backend,
        solar_os_inference_model_info_t *info);
    int (*run)(void *backend, const solar_os_inference_input_t *inputs, size_t count,
        solar_os_inference_cancel_fn cancel, void *user,
        solar_os_inference_result_t *result);
    int (*reset)(void *backend);
    int (*set_mode)(void *backend, solar_os_inference_mode_t mode);
    int (*close)(void *backend);
    /* Optional ownership transfer avoids a second model-file allocation.
     * allocation comes from host memory.alloc(bytes+31); data is its aligned
     * 16-byte start, at most 15 bytes after allocation. The provider consumes
     * allocation on every return, including failure. No borrowed bytes escape. */
    int (*load_owned_buffer)(void *allocation, uint8_t *data, size_t bytes,
        solar_os_inference_cancel_fn cancel, void *user, void **backend,
        solar_os_inference_model_info_t *info);
} solar_os_inference_backend_api_v1_t;

#define SOLAR_OS_INFERENCE_BACKEND_API_V1_SIZE \
    offsetof(solar_os_inference_backend_api_v1_t, load_owned_buffer)
