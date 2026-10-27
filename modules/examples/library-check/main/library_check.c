#include <string.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "hello_library_api.h"

static const solar_os_native_host_api_v1_t *host;
static const solar_os_native_library_service_api_v1_t *libraries;
static uint32_t leases[3];
static int check(int shared)
{
    const void *first = NULL, *second = NULL;
    uint32_t rejected = 99;
    const void *bad = (const void *)1;
    if (libraries->acquire("missing-library", 1, sizeof(hello_library_api_v1_t),
        &rejected, &bad) != SOLAR_OS_NATIVE_ERROR_NOT_FOUND || rejected || bad) return 10;
    for (unsigned cycle = 0; cycle < 25; ++cycle) {
        if (libraries->acquire("hello-library", HELLO_LIBRARY_ABI,
            sizeof(hello_library_api_v1_t), &leases[0], &first)) return 11;
        rejected = 99; bad = (const void *)1;
        if (libraries->acquire("hello-library", 2, sizeof(hello_library_api_v1_t),
            &rejected, &bad) != SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED || rejected || bad) return 12;
        if (libraries->acquire("hello-library", 1, sizeof(hello_library_api_v1_t) + 1,
            &rejected, &bad) != SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED || rejected || bad) return 13;
        if (libraries->acquire("hello-library", 1, sizeof(hello_library_api_v1_t),
            &leases[1], &second) || first != second || leases[0] == leases[1]) return 14;
        const hello_library_api_v1_t *api = first;
        if (api->add(19, 23) != 42 || api->add(UINT32_MAX, 1) != 0) return 15;
        if (libraries->retain(leases[0], &leases[2]) || leases[2] == leases[0]) return 16;
        if (libraries->unload("hello-library") != SOLAR_OS_NATIVE_ERROR_INVALID_STATE) return 17;
        uint32_t stale = leases[0];
        if (libraries->release(stale)) return 18;
        leases[0] = 0;
        if (libraries->release(stale) != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return 19;
        if (libraries->retain(stale, &rejected) != SOLAR_OS_NATIVE_ERROR_NOT_FOUND || rejected) return 20;
        if (libraries->release(leases[1])) return 21;
        leases[1] = 0;
        if (libraries->unload("hello-library") != SOLAR_OS_NATIVE_ERROR_INVALID_STATE ||
            api->add(cycle, 1) != cycle + 1) return 22;
        if (libraries->release(leases[2])) return 23;
        leases[2] = 0;
        if (shared) {
            if (libraries->unload("hello-library") != SOLAR_OS_NATIVE_ERROR_INVALID_STATE) return 24;
        } else {
            if (libraries->unload("hello-library")) return 24;
            if (libraries->unload("hello-library") != SOLAR_OS_NATIVE_ERROR_NOT_FOUND) return 25;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int shared = argc == 2 && strlen(argv[1]) == 6 && !memcmp(argv[1], "shared", 6);
    host = solar_os_native_host_v1();
    if (!host || host->struct_size < sizeof(*host) || !host->get_service || !host->write_utf8) return 1;
    libraries = host->get_service(SOLAR_OS_NATIVE_LIBRARY_SERVICE,
                                  SOLAR_OS_NATIVE_LIBRARY_ABI, sizeof(*libraries));
    if (!libraries || host->get_service("library", 2, sizeof(*libraries)) ||
        host->get_service("library", 1, sizeof(*libraries) + 1)) return 2;
    int result = check(shared);
    for (unsigned i = 0; i < 3; ++i) {
        if (leases[i]) { libraries->release(leases[i]); leases[i] = 0; }
    }
    if (!shared) libraries->unload("hello-library");
    const char *message = result ? "LIBRARY_CHECK_FAILED\n" : "LIBRARY_CHECK_OK 25 cycles\n";
    host->write_utf8(message, strlen(message));
    return result;
}
