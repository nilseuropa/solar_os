#include "solar_os_native_library_abi.h"
#include "hello_library_api.h"

static uint32_t hello_add(uint32_t left, uint32_t right) { return left + right; }
static int hello_shutdown(void) { return SOLAR_OS_NATIVE_OK; }
static const hello_library_api_v1_t api = {
    .abi_version = HELLO_LIBRARY_ABI, .struct_size = sizeof(api), .add = hello_add,
};
static const solar_os_native_library_descriptor_v1_t descriptor = {
    .abi_version = SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI,
    .struct_size = sizeof(descriptor),
    .id = "hello-library", .summary = "resident unsigned addition library",
    .api = (const solar_os_native_library_api_header_v1_t *)&api,
    .shutdown = hello_shutdown,
};

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_library_host_api_v1_t *host = solar_os_native_library_host_v1();
    if (!host || host->abi_version != SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI ||
        host->struct_size < sizeof(*host) || !host->register_library) return 1;
    return host->register_library(&descriptor);
}
