#pragma once

#include "solar_os_native_service_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI 1U
#define SOLAR_OS_NATIVE_LIBRARY_SERVICE "library"
#define SOLAR_OS_NATIVE_LIBRARY_ABI 1U

/* Every provider table starts with these fields. Provider ABIs are independent
 * of the library lifecycle ABI. Extend tables by appending fields. */
typedef struct {
    uint32_t abi_version, struct_size;
} solar_os_native_library_api_header_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    const char *id;
    const char *summary;
    const solar_os_native_library_api_header_v1_t *api;
    /* Called with no outstanding leases. Join workers, release dependencies
     * and allocations before returning OK. Failure leaves the library loaded.
     * Registration must not start workers or acquire resources. */
    int (*shutdown)(void);
} solar_os_native_library_descriptor_v1_t;

typedef struct {
    uint32_t abi_version, struct_size;
    const char *target;
    const char *firmware_version;
    int (*register_library)(const solar_os_native_library_descriptor_v1_t *descriptor);
    const void *(*get_service)(const char *name, uint32_t abi_version,
                               uint32_t minimum_struct_size);
} solar_os_native_library_host_api_v1_t;

/* Results use solar_os_native_service_result_v1_t. A successful acquire loads
 * an installed library if necessary, without network access, and returns a
 * unique lease. retain creates an independent lease for a client or worker.
 * Keep a lease until all provider calls, objects and callbacks using its code
 * are finished. release invalidates only that lease; double release is refused.
 * API pointers become invalid after the last lease is released and unload
 * succeeds. Zero is never a valid lease. Missing/wrong ABI returns no API. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*acquire)(const char *id, uint32_t abi_version, uint32_t minimum_struct_size,
                   uint32_t *lease, const void **api);
    int (*retain)(uint32_t lease, uint32_t *retained_lease);
    int (*release)(uint32_t lease);
    int (*unload)(const char *id);
} solar_os_native_library_service_api_v1_t;

const solar_os_native_library_host_api_v1_t *solar_os_native_library_host_v1(void);

#ifdef __cplusplus
}
#endif
