#include "solar_os_native_m3u.h"
#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_m3u_abi.h"
#if SOLAR_OS_PACKAGE_SERVICE_M3U
#include "solar_os_m3u.h"
#include "solar_os_native_support.h"
static int m3u_read(const char *path, solar_os_native_m3u_accept_fn accept, void *user)
{
    /* Core propagates callback errors. Native errors are negative; Core's
     * own errors are positive except ESP_FAIL, identical to native FAILED. */
    const int result = solar_os_m3u_read(path, accept, user);
    return result < 0 ? result : solar_os_native_result(result);
}
static int m3u_write(const char *path, size_t count, solar_os_m3u_get_fn get, void *user)
{ return solar_os_native_result(solar_os_m3u_write(path, count, get, user)); }
static const solar_os_native_m3u_api_v1_t m3u_api = {
    .abi_version = SOLAR_OS_NATIVE_M3U_ABI, .struct_size = sizeof(m3u_api),
    .path = solar_os_m3u_path, .read = m3u_read, .write = m3u_write,
};
#endif
const void *solar_os_native_m3u_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_m3u_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
