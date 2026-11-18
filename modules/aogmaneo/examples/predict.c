#include <stdio.h>
#include <aogmaneo/solar_os_aogmaneo_abi.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_library_abi.h"

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (!host || host->abi_version != 1 || host->struct_size < sizeof(*host) ||
        !host->get_service) return 1;
    const solar_os_native_library_service_api_v1_t *libraries =
        host->get_service("library", 1, sizeof(*libraries));
    if (!libraries) return 1;
    uint32_t lease = 0, model = 0;
    const void *table = NULL;
    int result = libraries->acquire(SOLAR_OS_AOGMANEO_LIBRARY, SOLAR_OS_AOGMANEO_ABI,
        sizeof(solar_os_aogmaneo_api_v1_t), &lease, &table);
    if (result) return 1;
    const solar_os_aogmaneo_api_v1_t *api = table;
    const solar_os_aogmaneo_io_v1_t input = {
        {1, 1, 4}, SOLAR_OS_AOGMANEO_PREDICTION, 4, 1, 1, 0, 0, 0
    };
    const solar_os_aogmaneo_layer_v1_t layer = {{2, 2, 16}, 4, 1, 0, 1};
    result = api->create(&input, 1, &layer, 1, 12345, &model);
    for (int tick = 0; !result && tick < 200; ++tick) {
        int32_t category = tick % 4;
        const solar_os_aogmaneo_codes_v1_t values = {&category, 1};
        result = api->step(model, &values, 1, true, 0.0f, 0.0f);
    }
    if (!result) {
        int32_t next = 0;
        result = api->prediction(model, 0, &next, 1);
        if (!result) printf("Predicted next category: %ld\n", (long)next);
    }
    if (model && api->destroy(model)) return 1;
    if (libraries->release(lease)) return 1;
    return result ? 1 : 0;
}
