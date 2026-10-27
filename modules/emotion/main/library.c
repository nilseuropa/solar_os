#include "solar_os_native_library_abi.h"
#include "solar_os_emotion_abi.h"

const solar_os_emotion_api_v1_t *solar_os_emotion_engine_api_v1(void);
static int shutdown(void) { return SOLAR_OS_NATIVE_OK; }
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_library_host_api_v1_t *host = solar_os_native_library_host_v1();
    if (!host || host->abi_version != SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI ||
        host->struct_size < sizeof(*host) || !host->register_library) return 1;
    const solar_os_native_library_descriptor_v1_t descriptor = {
        .abi_version = SOLAR_OS_NATIVE_LIBRARY_LIFECYCLE_ABI,
        .struct_size = sizeof(descriptor), .id = SOLAR_OS_EMOTION_LIBRARY,
        .summary = "Procedural facial expressions and reaction sounds",
        .api = (const solar_os_native_library_api_header_v1_t *)solar_os_emotion_engine_api_v1(),
        .shutdown = shutdown,
    };
    return host->register_library(&descriptor);
}
