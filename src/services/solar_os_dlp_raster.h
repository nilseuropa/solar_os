#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t x, y, width, height;
} solar_os_dlp_region_t;

/* DLP MONO1 rows are LSB-first and independently byte-padded. Find the changed
 * bounding rectangle, expanding x to byte boundaries (clipped at width).
 * A NULL previous image requests a full repaint. False means no pixel changed. */
bool solar_os_dlp_mono_region(const uint8_t *image, const uint8_t *previous,
    uint16_t width, uint16_t height, solar_os_dlp_region_t *region);

/* Keep vertically separated changes separate. Adjacent changed 8-row bands are
 * merged; if capacity is exhausted, remaining changes expand the last region.
 * capacity must be nonzero. A NULL previous image produces one full region. */
size_t solar_os_dlp_mono_regions(const uint8_t *image, const uint8_t *previous,
    uint16_t width, uint16_t height, solar_os_dlp_region_t *regions, size_t capacity);

/* Convert a region returned above to packed INDEX2, white=0 and black=3.
 * Caller supplies ceil(region.width/4)*region.height bytes. */
void solar_os_dlp_mono_index2(const uint8_t *image, uint16_t image_width,
    const solar_os_dlp_region_t *region, uint8_t *output);
