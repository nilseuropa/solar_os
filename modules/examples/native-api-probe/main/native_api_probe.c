#include "solar_os_native_client.h"

/* This source uses only the public SDK. The very same binary must work with
 * optional services present or absent; no board/flavor header is available. */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_get(
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_host_api_v1_t, get_service));
    if (!host ||
        !host->get_service || !host->write_utf8) return 1;
    const solar_os_native_system_api_v1_t *system = solar_os_native_system_get(
        host->get_service, SOLAR_OS_NATIVE_THROUGH(solar_os_native_system_api_v1_t, service_info));
    if (!solar_os_native_system_has_service_count(system) ||
        !solar_os_native_system_has_service_info(system)) return 2;
    const solar_os_native_time_api_v1_t *time = solar_os_native_time_get(host->get_service,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_time_api_v1_t, sleep_ms));
    if (!solar_os_native_time_has_now_us(time) || !solar_os_native_time_has_sleep_ms(time)) return 3;
    (void)time->now_us();
    time->sleep_ms(0);
    for (uint32_t index = 0; index < system->service_count(); ++index) {
        solar_os_native_system_service_v1_t info;
        if (system->service_info(index, &info)) return 4;
        const void *api = host->get_service(info.name, info.abi_version, 8);
        if ((api != NULL) != ((info.flags & SOLAR_OS_NATIVE_SYSTEM_AVAILABLE) != 0)) return 5;
        if (api && !solar_os_native_api_matches(api, info.abi_version, info.struct_size)) return 6;
        if (host->get_service(info.name, UINT32_MAX, 8) ||
            host->get_service(info.name, info.abi_version, UINT32_MAX)) return 7;
    }
    if (host->get_service("solaros.unknown", 1, 8)) return 8;
    /* A missing appended method is ordinary capability discovery. Never read
     * it before testing its complete prefix, including on older firmware. */
    const solar_os_native_memory_api_v1_t *memory = solar_os_native_memory_get(host->get_service,
        SOLAR_OS_NATIVE_THROUGH(solar_os_native_memory_api_v1_t, free));
    if (solar_os_native_memory_has_alloc(memory) && solar_os_native_memory_has_free(memory)) {
        void *buffer = memory->alloc(64);
        if (!buffer) return 9;
        memory->free(buffer);
    }
    if (host->write_utf8("NATIVE_API_PROBE_OK\n", 20)) return 10;
    return 0;
}
