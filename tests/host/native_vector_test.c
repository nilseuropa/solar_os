#include <assert.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_vector.h"
#include "solar_os_vector.h"

int main(void)
{
    const solar_os_native_vector_api_v1_t *api = solar_os_native_vector_get(
        solar_os_native_vector_get_service, sizeof(*api));
#if SOLAR_OS_PACKAGE_SERVICE_VECTOR
    assert(api && api->struct_size == sizeof(*api));
    /* The existing decoder calls the same kernels through its callback slot;
     * ABI access adds no allocation or extra conversion/copy in that path. */
    assert(api->fill_rgb565_be == solar_os_vector_fill_rgb565_be);
    assert(api->expand_1bpp_to_rgb565_be == solar_os_vector_expand_1bpp_to_rgb565_be);
    assert(api->rgba_to_gray_scaled == solar_os_vector_rgba_to_gray_scaled);
    assert(api->rgba_to_rgb_scaled == solar_os_vector_rgba_to_rgb_scaled);
    uint8_t pixels[16];
    memset(pixels, 0xa5, sizeof(pixels));
    api->fill_rgb565_be(pixels, 0x1234, 4);
    assert(!memcmp(pixels, "\x12\x34\x12\x34\x12\x34\x12\x34", 8) && pixels[8] == 0xa5);
    const uint8_t columns[] = {0, 2, 0, 2};
    api->expand_1bpp_to_rgb565_be(pixels, columns, 1, 0x1234, 0xabcd, 4);
    assert(!memcmp(pixels, "\x12\x34\xab\xcd\x12\x34\xab\xcd", 8) && pixels[8] == 0xa5);
    const uint8_t rgba[] = {255,0,0,255, 0,255,0,255, 0,0,255,127, 20,30,40,128};
    api->rgba_to_gray_scaled(pixels, 2, 2, rgba, 2, 2);
    assert(pixels[0] == 76 && pixels[1] == 149 && pixels[2] == 255 && pixels[3] == 28);
    api->rgba_to_rgb_scaled(pixels, 2, 2, rgba, 2, 2);
    const uint8_t rgb[] = {255,0,0, 0,255,0, 255,255,255, 20,30,40};
    assert(!memcmp(pixels, rgb, sizeof(rgb)));
    uint8_t single[3];
    api->rgba_to_rgb_scaled(single, 1, 1, rgba, 2, 2);
    assert(!memcmp(single, rgb, 3));
    memset(single, 0xa5, sizeof(single));
    api->rgba_to_rgb_scaled(single, 0, 1, rgba, 2, 2);
    assert(single[0] == 0xa5);
    api->fill_rgb565_be(NULL, 0, 1);
    api->expand_1bpp_to_rgb565_be(NULL, NULL, 1, 0, 0, 1);
    assert(!solar_os_native_vector_get_service("vector", 2, 8));
    assert(!solar_os_native_vector_get_service("vector", 1, sizeof(*api) + 1));
#else
    assert(!api);
#endif
    assert(!solar_os_native_vector_get_service(NULL, 1, 8));
    return 0;
}
