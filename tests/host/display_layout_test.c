#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    test_split_geometry();
    test_join_geometry();
    test_split_blit_preserves_sibling();
    puts("display layout tests passed");
    return 0;
}
