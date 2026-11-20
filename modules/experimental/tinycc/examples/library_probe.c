/* This source uses only the published SolarOS host and library ABI. */
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"
#include "hello_library_api.h"

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    const solar_os_native_library_service_api_v1_t *libraries;
    const hello_library_api_v1_t *api;
    const void *table = NULL;
    uint32_t lease = 0, value;
    int result;
    (void)argc; (void)argv;
    if (!host || host->abi_version != SOLAR_OS_NATIVE_ABI_VERSION ||
        host->struct_size < sizeof(*host) || !host->get_service) return 1;
    libraries = host->get_service("library", SOLAR_OS_NATIVE_LIBRARY_ABI,
                                  sizeof(*libraries));
    if (!libraries) return 2;
    result = libraries->acquire("hello-library", HELLO_LIBRARY_ABI,
                                sizeof(*api), &lease, &table);
    if (result) return 3;
    api = table;
    value = api->add(19, 23);
    result = libraries->release(lease);
    if (result || value != 42) return 4;
    host->write_utf8("TinyCC: native library returned 42\n", 35);
    return 0;
}
