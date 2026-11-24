#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_display_layout_core.h"

static void test_split_geometry(void)
{
    solar_os_display_layout_rect_t regions[2];
    assert(solar_os_display_layout_split_geometry(
        SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL, 792U, 272U, regions));
    assert(regions[0].x == 0U && regions[0].width == 396U);
    assert(regions[1].x == 396U && regions[1].width == 396U);
    assert(regions[0].height == 272U && regions[1].height == 272U);

    assert(solar_os_display_layout_split_geometry(
        SOLAR_OS_DISPLAY_LAYOUT_VERTICAL, 101U, 55U, regions));
    assert(regions[0].height == 27U);
    assert(regions[1].y == 27U && regions[1].height == 28U);
    assert(!solar_os_display_layout_split_geometry(
        SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL, 1U, 10U, regions));
}

static void test_join_geometry(void)
{
    const uint16_t widths[] = {128U, 84U, 240U};
    const uint16_t heights[] = {64U, 48U, 135U};
    solar_os_display_layout_rect_t regions[3];
    uint16_t width = 0U;
    uint16_t height = 0U;

    assert(solar_os_display_layout_join_geometry(
        SOLAR_OS_DISPLAY_LAYOUT_HORIZONTAL, widths, heights, 3U,
        &width, &height, regions));
    assert(width == 452U && height == 135U);
    assert(regions[0].x == 0U && regions[1].x == 128U &&
           regions[2].x == 212U);

    assert(solar_os_display_layout_join_geometry(
        SOLAR_OS_DISPLAY_LAYOUT_VERTICAL, widths, heights, 3U,
        &width, &height, regions));
    assert(width == 240U && height == 247U);
    assert(regions[0].y == 0U && regions[1].y == 64U &&
           regions[2].y == 112U);
}

static void test_split_blit_preserves_sibling(void)
{
    const uint16_t canvas_width = 792U;
    const uint16_t canvas_height = 272U;
    const uint16_t child_width = 396U;
    const size_t canvas_size = solar_os_display_layout_mono_size(
        canvas_width, canvas_height);
    const size_t child_size = solar_os_display_layout_mono_size(
        child_width, canvas_height);
    uint8_t *canvas = calloc(1U, canvas_size);
    uint8_t *left = calloc(1U, child_size);
    uint8_t *right = calloc(1U, child_size);
    assert(canvas != NULL && left != NULL && right != NULL);

    solar_os_display_layout_mono_set(left, child_width, canvas_height,
                                     395U, 271U, true);
    solar_os_display_layout_mono_set(right, child_width, canvas_height,
                                     0U, 0U, true);
    assert(solar_os_display_layout_mono_blit(
        canvas, canvas_width, canvas_height, 0U, 0U,
        left, child_width, canvas_height));
    assert(solar_os_display_layout_mono_blit(
        canvas, canvas_width, canvas_height, 396U, 0U,
        right, child_width, canvas_height));
    assert(solar_os_display_layout_mono_get(
        canvas, canvas_width, canvas_height, 395U, 271U));
    assert(solar_os_display_layout_mono_get(
        canvas, canvas_width, canvas_height, 396U, 0U));

    solar_os_display_layout_mono_set(left, child_width, canvas_height,
                                     395U, 271U, false);
    solar_os_display_layout_mono_set(left, child_width, canvas_height,
                                     1U, 1U, true);
    assert(solar_os_display_layout_mono_blit(
        canvas, canvas_width, canvas_height, 0U, 0U,
        left, child_width, canvas_height));
    assert(!solar_os_display_layout_mono_get(
        canvas, canvas_width, canvas_height, 395U, 271U));
    assert(solar_os_display_layout_mono_get(
        canvas, canvas_width, canvas_height, 1U, 1U));
    assert(solar_os_display_layout_mono_get(
        canvas, canvas_width, canvas_height, 396U, 0U));

    free(right);
    free(left);
    free(canvas);
}

static void fill_pattern(uint8_t *buffer, size_t size, unsigned seed)
{
    for (size_t i = 0; i < size; i++) {
        seed = seed * 1664525U + 1013904223U;
        buffer[i] = (uint8_t)(seed >> 24U);
    }
}

static void test_packed_copies_match_pixel_reference(void)
{
    const uint16_t widths[] = {1U, 7U, 8U, 9U, 101U, 396U, 792U};
    const uint16_t heights[] = {1U, 7U, 8U, 9U, 27U, 136U, 272U};
    for (size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++) {
        for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
            const uint16_t width = widths[w], height = heights[h];
            const uint16_t canvas_width = width + 7U, canvas_height = height + 9U;
            const size_t size = solar_os_display_layout_mono_size(width, height);
            const size_t canvas_size = solar_os_display_layout_mono_size(canvas_width, canvas_height);
            uint8_t *source = malloc(size), *canvas = malloc(canvas_size);
            uint8_t *expected = malloc(canvas_size), *extracted = malloc(size);
            uint8_t *expected_extract = malloc(size);
            assert(source && canvas && expected && extracted && expected_extract);
            fill_pattern(source, size, 19U);
            for (uint16_t offset_y = 0U; offset_y < 8U; offset_y++) {
                fill_pattern(canvas, canvas_size, offset_y + 1U);
                memcpy(expected, canvas, canvas_size);
                for (uint16_t y = 0U; y < height; y++) {
                    for (uint16_t x = 0U; x < width; x++) {
                        solar_os_display_layout_mono_set(expected,
                            canvas_width, canvas_height, x + 3U, y + offset_y,
                            solar_os_display_layout_mono_get(source, width, height, x, y));
                    }
                }
                assert(solar_os_display_layout_mono_blit(canvas,
                    canvas_width, canvas_height, 3U, offset_y, source, width, height));
                /* Includes sibling pixels and padding outside the copied area. */
                assert(memcmp(canvas, expected, canvas_size) == 0);

                fill_pattern(canvas, canvas_size, offset_y + 41U);
                fill_pattern(extracted, size, 97U);
                memcpy(expected_extract, extracted, size);
                for (uint16_t y = 0U; y < height; y++) {
                    for (uint16_t x = 0U; x < width; x++) {
                        solar_os_display_layout_mono_set(expected_extract, width, height, x, y,
                            solar_os_display_layout_mono_get(canvas,
                                canvas_width, canvas_height, x + 3U, y + offset_y));
                    }
                }
                assert(solar_os_display_layout_mono_extract(extracted, width, height,
                    canvas, canvas_width, canvas_height, 3U, offset_y));
                assert(memcmp(extracted, expected_extract, size) == 0);
            }
            free(expected_extract); free(extracted); free(expected); free(canvas); free(source);
        }
    }
}

static void test_invalid_copy_bounds(void)
{
    uint8_t source[16] = {0}, destination[16];
    memset(destination, 0xa5, sizeof(destination));
    assert(!solar_os_display_layout_mono_blit(destination, 8U, 8U, 1U, 0U, source, 8U, 8U));
    assert(!solar_os_display_layout_mono_blit(destination, 8U, 8U, 0U, 1U, source, 8U, 8U));
    assert(!solar_os_display_layout_mono_blit(destination, 8U, 8U, 0U, 0U, source, 0U, 8U));
    assert(!solar_os_display_layout_mono_extract(destination, 8U, 8U, source, 8U, 8U, 1U, 0U));
    assert(!solar_os_display_layout_mono_extract(destination, 8U, 8U, source, 8U, 8U, 0U, 1U));
    assert(!solar_os_display_layout_mono_extract(destination, 8U, 8U, source, 8U, 8U, UINT16_MAX, 0U));
    assert(!solar_os_display_layout_mono_extract(destination, 8U, 8U, NULL, 8U, 8U, 0U, 0U));
    for (size_t i = 0U; i < sizeof(destination); i++) assert(destination[i] == 0xa5);
}

int main(void)
{
    test_split_geometry();
    test_join_geometry();
    test_split_blit_preserves_sibling();
    test_packed_copies_match_pixel_reference();
    test_invalid_copy_bounds();
    puts("display layout tests passed");
    return 0;
}
