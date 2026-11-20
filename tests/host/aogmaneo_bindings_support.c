#include "solar_os_aogmaneo.h"
#include "solar_os_native.h"
#include <assert.h>
#include <string.h>

extern const solar_os_aogmaneo_api_v1_t *solar_os_aogmaneo_engine_api_v1(void);
extern bool solar_os_aogmaneo_engine_idle(void);
int aon_test_leases, aon_test_missing, aon_test_release_failures, aon_test_destroy_failures;
static uint32_t next_lease;
static bool leases[256];
static solar_os_aogmaneo_api_v1_t table;
static int test_destroy(uint32_t model)
{
    if (aon_test_destroy_failures) {
        --aon_test_destroy_failures;
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    return solar_os_aogmaneo_engine_api_v1()->destroy(model);
}
int solar_os_native_library_acquire(const char *id, uint32_t abi, uint32_t size,
    uint32_t *lease, const void **api)
{
    assert(!strcmp(id, "aogmaneo") && abi == 1 && size == sizeof(table));
    *lease = 0; *api = NULL;
    if (aon_test_missing) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    table = *solar_os_aogmaneo_engine_api_v1();
    table.destroy = test_destroy;
    assert(++next_lease < 256);
    leases[next_lease] = true;
    ++aon_test_leases;
    *lease = next_lease; *api = &table;
    return 0;
}
int solar_os_native_library_release(uint32_t lease)
{
    assert(lease && lease < 256 && leases[lease]);
    if (aon_test_release_failures) {
        --aon_test_release_failures;
        return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    leases[lease] = false;
    --aon_test_leases;
    return 0;
}
void aon_test_clean(void)
{ assert(!aon_test_leases && solar_os_aogmaneo_engine_idle()); }
