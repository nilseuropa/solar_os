#include "solar_os_native_vector.h"

#include <string.h>
#include "solar_os_config.h"
#include "solar_os_native_vector_abi.h"
#include "solar_os_vector.h"

#if SOLAR_OS_PACKAGE_SERVICE_VECTOR
static const solar_os_native_vector_api_v1_t vector_api = {
    .abi_version = SOLAR_OS_NATIVE_VECTOR_ABI, .struct_size = sizeof(vector_api),
    .fill_rgb565_be = solar_os_vector_fill_rgb565_be,
    .expand_1bpp_to_rgb565_be = solar_os_vector_expand_1bpp_to_rgb565_be,
    .rgba_to_gray_scaled = solar_os_vector_rgba_to_gray_scaled,
    .rgba_to_rgb_scaled = solar_os_vector_rgba_to_rgb_scaled,
};
#endif

const void *solar_os_native_vector_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_vector_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
