#include "solar_os_robot_abi.h"
#include "solar_os_native_time_abi.h"
#include "solar_os_native_thread_abi.h"
#include "solar_os_native_controllers_abi.h"

const solar_os_robot_api_v3_t *solar_os_robot_engine_init(
    const solar_os_native_memory_api_v1_t *, const solar_os_native_time_api_v1_t *,
    const solar_os_native_thread_api_v1_t *, const solar_os_native_motion_api_v1_t *,
    const solar_os_native_controllers_api_v1_t *);
int solar_os_robot_engine_shutdown(void);

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_library_host_api_v1_t *host = solar_os_native_library_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) || !host->get_service || !host->register_library) return 1;
    const solar_os_native_memory_api_v1_t *memory = host->get_service("memory", 1, sizeof(*memory));
    const solar_os_native_time_api_v1_t *time = host->get_service("time", 1, sizeof(*time));
    const solar_os_native_thread_api_v1_t *thread = host->get_service("thread", 1, sizeof(*thread));
    const solar_os_native_motion_api_v1_t *motion = host->get_service("motion", 1, sizeof(*motion));
    const solar_os_native_controllers_api_v1_t *controllers=host->get_service("controllers",1,sizeof(*controllers));
    const solar_os_robot_api_v3_t *api = solar_os_robot_engine_init(memory, time, thread, motion,controllers);
    if (!api) return 1;
    const solar_os_native_library_descriptor_v1_t descriptor = {
        .abi_version = 1, .struct_size = sizeof(descriptor), .id = SOLAR_OS_ROBOT_LIBRARY,
        .summary = "Robot model and joint runtime", .api = (const solar_os_native_library_api_header_v1_t *)api,
        .shutdown = solar_os_robot_engine_shutdown,
    };
    int result = host->register_library(&descriptor);
    if (result != SOLAR_OS_NATIVE_OK) (void)solar_os_robot_engine_shutdown();
    return result;
}
