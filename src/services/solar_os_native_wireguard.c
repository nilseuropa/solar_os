#include "solar_os_native_wireguard.h"
#include "solar_os_config.h"
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_WIREGUARD
#include "solar_os_native_support.h"
#include "solar_os_wireguard.h"

static int import_profile(const char *path, char *detail, size_t capacity)
{
    if (detail && capacity) detail[0] = 0;
    if (!path || !*path || (!detail && capacity)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return solar_os_native_result(solar_os_wireguard_import(path, detail, capacity));
}

static int forget(void)
{ return solar_os_native_result(solar_os_wireguard_forget()); }

static int up(solar_os_wireguard_policy_t policy)
{ return solar_os_native_result(solar_os_wireguard_up(policy)); }

static int down(void)
{ return solar_os_native_result(solar_os_wireguard_down()); }

static int status(solar_os_wireguard_status_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = (solar_os_wireguard_status_t){0};
    solar_os_wireguard_get_status(out);
    out->last_error = solar_os_native_result(out->last_error);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_wireguard_api_v1_t wireguard_api = {
    .abi_version = SOLAR_OS_NATIVE_WIREGUARD_ABI, .struct_size = sizeof(wireguard_api),
    .import_profile = import_profile, .forget = forget, .up = up, .down = down, .status = status,
    .state_name = solar_os_wireguard_state_name, .policy_name = solar_os_wireguard_policy_name,
};
#endif

const void *solar_os_native_wireguard_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_wireguard_dispatch.inc"
    (void)name; (void)abi; (void)minimum;
    return NULL;
}
