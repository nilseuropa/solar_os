#include "solar_os_native_display_raster.h"
#include "solar_os_config.h"

#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_DLP
#include "solar_os_dlp_raster.h"

static bool mono_raster_valid(const solar_os_native_mono_raster_v1_t *image)
{
    return image && image->pixels && image->width && image->height &&
        image->size >= ((image->width + 7U) / 8U) * (size_t)image->height;
}

static int mono_damage(const solar_os_native_mono_raster_v1_t *image,
    const solar_os_native_mono_raster_v1_t *previous, solar_os_native_raster_region_v1_t *regions,
    size_t capacity, size_t *count)
{
    if (!count) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count = 0;
    if (!mono_raster_valid(image) || !regions || !capacity ||
        (previous && (!mono_raster_valid(previous) || image->width != previous->width ||
                      image->height != previous->height))) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    *count = solar_os_dlp_mono_regions(image->pixels, previous ? previous->pixels : NULL,
        image->width, image->height, regions, capacity);
    return SOLAR_OS_NATIVE_OK;
}

static int mono_to_index2(const solar_os_native_mono_raster_v1_t *image,
    const solar_os_native_raster_region_v1_t *region, uint8_t *output, size_t capacity)
{
    if (!mono_raster_valid(image) || !region || !output ||
        !region->width || !region->height || (region->x % 8U) ||
        (uint32_t)region->x + region->width > image->width ||
        (uint32_t)region->y + region->height > image->height)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if (capacity < ((region->width + 3U) / 4U) * (size_t)region->height)
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_dlp_mono_index2(image->pixels, image->width, region, output);
    return SOLAR_OS_NATIVE_OK;
}

static const solar_os_native_display_raster_api_v1_t display_raster_api = {
    .abi_version = SOLAR_OS_NATIVE_DISPLAY_RASTER_ABI,
    .struct_size = sizeof(display_raster_api),
    .mono_damage = mono_damage,
    .mono_to_index2 = mono_to_index2,
};
#endif

const void *solar_os_native_display_raster_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_display_raster_dispatch.inc"
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
