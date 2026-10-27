#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_dlp_raster.h"

static void check_pixels(const uint8_t *image, unsigned width,
    const solar_os_dlp_region_t *region)
{
    size_t stride = (region->width + 3U) / 4U;
    size_t size = stride * region->height;
    uint8_t output[128];
    memset(output, 0xa5, sizeof(output));
    solar_os_dlp_mono_index2(image, width, region, output);
    for (unsigned y = 0; y < region->height; y++) {
        for (unsigned x = 0; x < stride * 4U; x++) {
            unsigned got = (output[y * stride + x / 4U] >> (2U * (x % 4U))) & 3U;
            unsigned source_x = region->x + x;
            unsigned expected = x < region->width &&
                (image[(region->y + y) * ((width + 7U) / 8U) + source_x / 8U] &
                 (1U << (source_x % 8U))) ? 3 : 0;
            assert(got == expected);
        }
    }
    assert(output[size] == 0xa5);
}

int main(void)
{
    solar_os_dlp_region_t region;
    for (unsigned value = 0; value < 256; value++) {
        uint8_t byte = value;
        assert(solar_os_dlp_mono_region(&byte, NULL, 8, 1, &region));
        check_pixels(&byte, 8, &region);
    }
    for (unsigned width = 1; width <= 33; width++) {
        unsigned stride = (width + 7U) / 8U;
        uint8_t image[35] = {0}, previous[35] = {0};
        assert(solar_os_dlp_mono_region(image, NULL, width, 7, &region));
        assert(region.x == 0 && region.y == 0 && region.width == width && region.height == 7);
        assert(!solar_os_dlp_mono_region(image, previous, width, 7, &region));
        for (unsigned x = 0; x < width; x++) {
            memset(image, 0, sizeof(image));
            image[2 * stride + x / 8U] = 1U << (x % 8U);
            image[5 * stride + x / 8U] = 1U << (x % 8U);
            assert(solar_os_dlp_mono_region(image, previous, width, 7, &region));
            unsigned right = (x / 8U + 1U) * 8U;
            if (right > width) right = width;
            assert(region.x == x / 8U * 8U && region.width == right - region.x);
            assert(region.y == 2 && region.height == 4);
            check_pixels(image, width, &region);
        }
        memset(image, 0xff, sizeof(image));
        assert(solar_os_dlp_mono_region(image, previous, width, 7, &region));
        check_pixels(image, width, &region);
        /* Padding bits do not change the visible image. */
        memcpy(previous, image, sizeof(image));
        if (width % 8U) {
            image[3 * stride + stride - 1] ^= 0xffU << (width % 8U);
            assert(!solar_os_dlp_mono_region(image, previous, width, 7, &region));
        }
    }
    uint8_t image[40] = {0}, previous[40] = {0};
    solar_os_dlp_region_t regions[16];
    image[1] = 1;  /* x=8,y=0 */
    image[38] = 1; /* x=0,y=19 */
    assert(solar_os_dlp_mono_regions(image, previous, 13, 20, regions, 16) == 2);
    assert(regions[0].x == 8 && regions[0].y == 0 && regions[0].width == 5 && regions[0].height == 1);
    assert(regions[1].x == 0 && regions[1].y == 19 && regions[1].width == 8 && regions[1].height == 1);
    assert(solar_os_dlp_mono_regions(image, previous, 13, 20, regions, 1) == 1);
    assert(regions[0].x == 0 && regions[0].width == 13 && regions[0].height == 20);
    memset(image, 0xff, sizeof(image));
    assert(solar_os_dlp_mono_regions(image, previous, 13, 20, regions, 16) == 1);
    assert(regions[0].width == 13 && regions[0].height == 20);
    /* Exhausting region storage still covers every changed pixel. */
    uint8_t sparse[384] = {0}, blank[384] = {0};
    for (unsigned y = 0; y < 192; y += 16) sparse[y * 2 + (y / 16) % 2] = 1;
    for (unsigned capacity = 1; capacity <= 16; capacity++) {
        size_t count = solar_os_dlp_mono_regions(sparse, blank, 13, 192, regions, capacity);
        assert(count <= capacity);
        for (unsigned y = 0; y < 192; y += 16) {
            unsigned x = ((y / 16) % 2) * 8;
            bool covered = false;
            for (size_t i = 0; i < count; i++)
                if (x >= regions[i].x && x < regions[i].x + regions[i].width &&
                    y >= regions[i].y && y < regions[i].y + regions[i].height) covered = true;
            assert(covered);
        }
    }
    puts("DLP batched conversion, all byte values, odd widths, crops and padding PASS");
    return 0;
}
