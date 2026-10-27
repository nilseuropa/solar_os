#include "solar_os_native_job_abi.h"
#include "solar_os_native_library_abi.h"
#include "hello_library_api.h"

static const solar_os_native_library_service_api_v1_t *libraries;
static const hello_library_api_v1_t *api;
static uint32_t client_lease, worker_lease, ticks;

static void stop(void)
{
    api = NULL;
    if (worker_lease) libraries->release(worker_lease);
    if (client_lease) libraries->release(client_lease);
    worker_lease = client_lease = 0;
}

static int start(int argc, char **argv)
{
    (void)argc; (void)argv;
    const void *table = NULL;
    int result = libraries->acquire("hello-library", HELLO_LIBRARY_ABI,
        sizeof(*api), &client_lease, &table);
    if (!result) result = libraries->retain(client_lease, &worker_lease);
    if (result) { stop(); return result; }
    api = table; ticks = 0;
    return 0;
}

static bool tick(uint32_t now_ms)
{
    (void)now_ms;
    if (api) ticks = api->add(ticks, 1);
    return false;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const solar_os_native_job_host_api_v1_t *host = solar_os_native_job_host_v1();
    if (!host || host->struct_size < sizeof(*host) || !host->get_service) return 1;
    libraries = host->get_service("library", 1, sizeof(*libraries));
    if (!libraries) return 2;
    static const solar_os_native_job_descriptor_v1_t descriptor = {
        .abi_version = SOLAR_OS_NATIVE_JOB_LIFECYCLE_ABI,
        .struct_size = sizeof(descriptor), .id = "library-user",
        .summary = "shared library client and worker lease example",
        .tick_interval_ms = 100, .tick_deadline_ms = 10,
        .start = start, .stop = stop, .tick = tick,
    };
    return host->register_job(&descriptor);
}
