#include "solar_os_native_library_abi.h"
#include "solar_os_processor_provider_abi.h"

const solar_os_processor_provider_api_v1_t *solar_os_himax_processor_api(void);
static int shutdown(void) { return SOLAR_OS_NATIVE_OK; }
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_library_host_api_v1_t *host = solar_os_native_library_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host)) return 1;
    const solar_os_native_library_descriptor_v1_t descriptor = {
        .abi_version = 1, .struct_size = sizeof(descriptor), .id = "processor-himax",
        .summary = "Himax HX6538 camera-bound inference",
        .api = (const solar_os_native_library_api_header_v1_t *)solar_os_himax_processor_api(),
        .shutdown = shutdown,
    };
    return host->register_library(&descriptor);
}
