#include "solar_os_gfx_internal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "solar_os_display.h"
#include "solar_os_fonts.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

#define GFX_INDEX8_PALETTE_SIZE 256U
#define GFX_INDEX8_COLOR_COUNT 216U
#define GFX_INDEX8_LITERAL_GRAY_COUNT 23U
#define GFX_INDEX8_THEME_COUNT 17U
#define GFX_INDEX8_THEME_BASE \
    (GFX_INDEX8_COLOR_COUNT + GFX_INDEX8_LITERAL_GRAY_COUNT)
#define GFX_DIRTY_TILE_SIZE 8U
#define GFX_OPEN_ICONIC_FIRST_ENCODING 64U

_Static_assert(GFX_INDEX8_THEME_BASE + GFX_INDEX8_THEME_COUNT ==
                   GFX_INDEX8_PALETTE_SIZE,
               "INDEX8 palette layout must fill all entries");
_Static_assert(SOLAR_OS_GFX_ICON_COUNT == 223,
               "Open Iconic mapping must name every glyph");

struct solar_os_gfx_index8_surface {
    solar_os_display_surface_t surface;
    uint8_t *pixels;
    uint8_t *dirty;
    uint16_t *palette;
    uint8_t draw_index;
    uint32_t theme_foreground_rgb888;
    uint32_t theme_background_rgb888;
};

static const char *TAG = "gfx";

static bool gfx_uses_index8(const solar_os_gfx_t *gfx);
static void gfx_mark_index8_all_dirty(solar_os_gfx_t *gfx);

static const uint8_t *gfx_font_data(solar_os_gfx_font_t font)
{
    switch (font) {
    case SOLAR_OS_GFX_FONT_SMALL:
        return u8g2_font_solar_os_default_r_12_tf;
    case SOLAR_OS_GFX_FONT_BOLD:
        return u8g2_font_solar_os_default_b_14_tf;
    case SOLAR_OS_GFX_FONT_MONO_12:
        return u8g2_font_solar_os_default_r_12_tf;
    case SOLAR_OS_GFX_FONT_MONO_14:
        return u8g2_font_solar_os_default_r_14_tf;
    case SOLAR_OS_GFX_FONT_MONO_16:
        return u8g2_font_solar_os_default_r_16_tf;
    case SOLAR_OS_GFX_FONT_MONO_18:
        return u8g2_font_solar_os_default_r_18_tf;
    case SOLAR_OS_GFX_FONT_MONO_20:
        return u8g2_font_solar_os_default_r_20_tf;
    case SOLAR_OS_GFX_FONT_BOLD_12:
        return u8g2_font_solar_os_default_b_12_tf;
    case SOLAR_OS_GFX_FONT_BOLD_14:
        return u8g2_font_solar_os_default_b_14_tf;
    case SOLAR_OS_GFX_FONT_BOLD_16:
        return u8g2_font_solar_os_default_b_16_tf;
    case SOLAR_OS_GFX_FONT_BOLD_18:
        return u8g2_font_solar_os_default_b_18_tf;
    case SOLAR_OS_GFX_FONT_BOLD_20:
        return u8g2_font_solar_os_default_b_20_tf;
    case SOLAR_OS_GFX_FONT_ITALIC_12:
        return u8g2_font_solar_os_default_i_12_tf;
    case SOLAR_OS_GFX_FONT_ITALIC_14:
        return u8g2_font_solar_os_default_i_14_tf;
    case SOLAR_OS_GFX_FONT_ITALIC_16:
        return u8g2_font_solar_os_default_i_16_tf;
    case SOLAR_OS_GFX_FONT_ITALIC_18:
        return u8g2_font_solar_os_default_i_18_tf;
    case SOLAR_OS_GFX_FONT_ITALIC_20:
        return u8g2_font_solar_os_default_i_20_tf;
    case SOLAR_OS_GFX_FONT_BOLD_ITALIC_12:
        return u8g2_font_solar_os_default_bi_12_tf;
    case SOLAR_OS_GFX_FONT_BOLD_ITALIC_14:
        return u8g2_font_solar_os_default_bi_14_tf;
    case SOLAR_OS_GFX_FONT_BOLD_ITALIC_16:
        return u8g2_font_solar_os_default_bi_16_tf;
    case SOLAR_OS_GFX_FONT_BOLD_ITALIC_18:
        return u8g2_font_solar_os_default_bi_18_tf;
    case SOLAR_OS_GFX_FONT_BOLD_ITALIC_20:
        return u8g2_font_solar_os_default_bi_20_tf;
    case SOLAR_OS_GFX_FONT_MONO:
    default:
        return u8g2_font_solar_os_default_r_14_tf;
    }
}

static const uint8_t *gfx_icon_font_data(solar_os_gfx_icon_size_t size)
{
    switch (size) {
    case SOLAR_OS_GFX_ICON_SIZE_8:
        return u8g2_font_open_iconic_all_1x_t;
    case SOLAR_OS_GFX_ICON_SIZE_16:
        return u8g2_font_open_iconic_all_2x_t;
    case SOLAR_OS_GFX_ICON_SIZE_32:
        return u8g2_font_open_iconic_all_4x_t;
    case SOLAR_OS_GFX_ICON_SIZE_48:
        return u8g2_font_open_iconic_all_6x_t;
    case SOLAR_OS_GFX_ICON_SIZE_64:
        return u8g2_font_open_iconic_all_8x_t;
    default:
        return NULL;
    }
}

static bool gfx_color_is_gray(solar_os_gfx_color_t color)
{
    const unsigned value = (unsigned)color;
    return value >= SOLAR_OS_GFX_COLOR_GRAY_BASE && value <= SOLAR_OS_GFX_COLOR_GRAY_LAST;
}

static bool gfx_color_is_rgb(solar_os_gfx_color_t color)
{
    return (color & 0xff000000UL) == SOLAR_OS_GFX_COLOR_RGB_FLAG;
}

static uint32_t gfx_color_rgb888(solar_os_gfx_color_t color)
{
    if (gfx_color_is_rgb(color)) {
        return color & 0x00ffffffUL;
    }
    uint8_t level = 0;
    if (gfx_color_is_gray(color)) {
        level = (uint8_t)((color - SOLAR_OS_GFX_COLOR_GRAY_BASE) * 255U /
                          SOLAR_OS_GFX_GRAY_MAX);
    } else {
        switch (color) {
        case SOLAR_OS_GFX_COLOR_WHITE: level = 255U; break;
        case SOLAR_OS_GFX_COLOR_LIGHT: level = 191U; break;
        case SOLAR_OS_GFX_COLOR_DARK: level = 80U; break;
        case SOLAR_OS_GFX_COLOR_BLACK:
        default: level = 0U; break;
        }
    }
    return ((uint32_t)level << 16U) | ((uint32_t)level << 8U) | level;
}

static uint8_t gfx_dither_threshold(solar_os_gfx_color_t color)
{
    if (gfx_color_is_rgb(color)) {
        const uint32_t rgb = gfx_color_rgb888(color);
        const uint32_t red = (rgb >> 16U) & 0xffU;
        const uint32_t green = (rgb >> 8U) & 0xffU;
        const uint32_t blue = rgb & 0xffU;
        const uint32_t luminance = (red * 77U + green * 150U + blue * 29U) >> 8U;
        return (uint8_t)((luminance * 16U + 127U) / 255U);
    }
    if (gfx_color_is_gray(color)) {
        return (uint8_t)((unsigned)color - SOLAR_OS_GFX_COLOR_GRAY_BASE);
    }

    switch (color) {
    case SOLAR_OS_GFX_COLOR_WHITE:
        return 16;
    case SOLAR_OS_GFX_COLOR_LIGHT:
        return 12;
    case SOLAR_OS_GFX_COLOR_DARK:
        return 5;
    case SOLAR_OS_GFX_COLOR_BLACK:
    default:
        return 0;
    }
}

static uint16_t gfx_rgb888_to_rgb565(uint32_t rgb888)
{
    return (uint16_t)((((rgb888 >> 19U) & 0x1fU) << 11U) |
                      (((rgb888 >> 10U) & 0x3fU) << 5U) |
                      ((rgb888 >> 3U) & 0x1fU));
}

static uint8_t gfx_index8_for_rgb888(uint32_t rgb888)
{
    const uint8_t red = (uint8_t)(rgb888 >> 16U);
    const uint8_t green = (uint8_t)(rgb888 >> 8U);
    const uint8_t blue = (uint8_t)rgb888;
    if (red == green && green == blue) {
        return (uint8_t)(GFX_INDEX8_COLOR_COUNT +
            ((unsigned)red * (GFX_INDEX8_LITERAL_GRAY_COUNT - 1U) + 127U) /
                255U);
    }
    const unsigned red_level = ((unsigned)red * 5U + 127U) / 255U;
    const unsigned green_level = ((unsigned)green * 5U + 127U) / 255U;
    const unsigned blue_level = ((unsigned)blue * 5U + 127U) / 255U;
    return (uint8_t)(red_level * 36U + green_level * 6U + blue_level);
}

static uint8_t gfx_index8_for_color(solar_os_gfx_color_t color)
{
    if (gfx_color_is_rgb(color)) {
        return gfx_index8_for_rgb888(gfx_color_rgb888(color));
    }

    const unsigned threshold = gfx_dither_threshold(color);
    return (uint8_t)(GFX_INDEX8_THEME_BASE + threshold);
}

static uint8_t gfx_blend_channel(uint8_t foreground,
                                 uint8_t background,
                                 unsigned background_weight)
{
    return (uint8_t)(((unsigned)foreground * (255U - background_weight) +
                      (unsigned)background * background_weight + 127U) / 255U);
}

static void gfx_build_index8_palette(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || gfx->index8 == NULL || gfx->index8->palette == NULL) {
        return;
    }
    for (unsigned index = 0; index < GFX_INDEX8_COLOR_COUNT; index++) {
        unsigned value = index;
        uint8_t blue = (uint8_t)((value % 6U) * 255U / 5U);
        value /= 6U;
        uint8_t green = (uint8_t)((value % 6U) * 255U / 5U);
        uint8_t red = (uint8_t)((value / 6U) * 255U / 5U);
        gfx->index8->palette[index] = gfx_rgb888_to_rgb565(
            ((uint32_t)red << 16U) | ((uint32_t)green << 8U) | blue);
    }
    for (unsigned gray = 0; gray < GFX_INDEX8_LITERAL_GRAY_COUNT; gray++) {
        const uint8_t level = (uint8_t)(
            gray * 255U / (GFX_INDEX8_LITERAL_GRAY_COUNT - 1U));
        gfx->index8->palette[GFX_INDEX8_COLOR_COUNT + gray] =
            gfx_rgb888_to_rgb565(((uint32_t)level << 16U) |
                                 ((uint32_t)level << 8U) | level);
    }

    uint32_t foreground = 0x000000U;
    uint32_t background = 0xffffffU;
    (void)solar_os_display_get_colors(&foreground, &background);
    gfx->index8->theme_foreground_rgb888 = foreground;
    gfx->index8->theme_background_rgb888 = background;
    if (gfx->palette_inverted) {
        const uint32_t swap = foreground;
        foreground = background;
        background = swap;
    }
    for (unsigned level = 0; level < GFX_INDEX8_THEME_COUNT; level++) {
        const unsigned background_weight =
            level * 255U / (GFX_INDEX8_THEME_COUNT - 1U);
        const uint8_t red = gfx_blend_channel((uint8_t)(foreground >> 16U),
                                              (uint8_t)(background >> 16U),
                                              background_weight);
        const uint8_t green = gfx_blend_channel((uint8_t)(foreground >> 8U),
                                                (uint8_t)(background >> 8U),
                                                background_weight);
        const uint8_t blue = gfx_blend_channel((uint8_t)foreground,
                                               (uint8_t)background,
                                               background_weight);
        gfx->index8->palette[GFX_INDEX8_THEME_BASE + level] =
            gfx_rgb888_to_rgb565(((uint32_t)red << 16U) |
                                 ((uint32_t)green << 8U) | blue);
    }
}

static void gfx_sync_index8_theme(solar_os_gfx_t *gfx)
{
    if (!gfx_uses_index8(gfx)) {
        return;
    }

    uint32_t foreground = 0;
    uint32_t background = 0;
    if (solar_os_display_get_colors(&foreground, &background) != ESP_OK ||
        (foreground == gfx->index8->theme_foreground_rgb888 &&
         background == gfx->index8->theme_background_rgb888)) {
        return;
    }

    gfx_build_index8_palette(gfx);
    gfx_mark_index8_all_dirty(gfx);
    gfx->dirty = true;
}

static bool gfx_uses_index8(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->index8 != NULL &&
        gfx->index8->surface.format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 &&
        gfx->index8->pixels != NULL;
}

static uint8_t gfx_apply_polarity(const solar_os_gfx_t *gfx, uint8_t white_bit)
{
    return gfx != NULL &&
        (gfx->black_is_one != gfx->palette_inverted) ?
        (uint8_t)!white_bit :
        white_bit;
}

static uint8_t gfx_binary_draw_color(const solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{
    return gfx_apply_polarity(gfx, gfx_dither_threshold(color) >= 8 ? 1 : 0);
}

static uint8_t gfx_pattern_draw_color(const solar_os_gfx_t *gfx,
                                      solar_os_gfx_color_t color,
                                      int x,
                                      int y)
{
    static const uint8_t bayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };

    const uint8_t threshold = gfx_dither_threshold(color);
    if (threshold == 0) {
        return gfx_apply_polarity(gfx, 0);
    }
    if (threshold >= 16) {
        return gfx_apply_polarity(gfx, 1);
    }

    return gfx_apply_polarity(gfx, bayer4[y & 3][x & 3] < threshold ? 1 : 0);
}

static bool gfx_ready(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->u8g2 != NULL;
}

static solar_os_display_rotation_t gfx_rotation(const u8g2_t *u8g2)
{
    if (u8g2 != NULL && u8g2->cb == U8G2_R1) {
        return SOLAR_OS_DISPLAY_ROTATION_90;
    }
    if (u8g2 != NULL && u8g2->cb == U8G2_R2) {
        return SOLAR_OS_DISPLAY_ROTATION_180;
    }
    if (u8g2 != NULL && u8g2->cb == U8G2_R3) {
        return SOLAR_OS_DISPLAY_ROTATION_270;
    }
    return SOLAR_OS_DISPLAY_ROTATION_0;
}

static void gfx_logical_to_native(const solar_os_display_surface_t *surface,
                                  int x,
                                  int y,
                                  int *native_x,
                                  int *native_y)
{
    int mapped_x = x;
    int mapped_y = y;
    switch (surface->rotation) {
    case SOLAR_OS_DISPLAY_ROTATION_90:
        mapped_x = (int)surface->native_width - 1 - y;
        mapped_y = x;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_180:
        mapped_x = (int)surface->native_width - 1 - x;
        mapped_y = (int)surface->native_height - 1 - y;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_270:
        mapped_x = y;
        mapped_y = (int)surface->native_height - 1 - x;
        break;
    case SOLAR_OS_DISPLAY_ROTATION_0:
    default:
        break;
    }
    *native_x = mapped_x;
    *native_y = mapped_y;
}

static void gfx_mark_index8_dirty_rect(solar_os_gfx_t *gfx,
                                       int x,
                                       int y,
                                       int width,
                                       int height)
{
    if (!gfx_uses_index8(gfx) || gfx->index8->dirty == NULL ||
        width <= 0 || height <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + width - 1;
    int y1 = y + height - 1;
    const solar_os_display_surface_t *surface = &gfx->index8->surface;
    if (x1 >= (int)surface->width) x1 = (int)surface->width - 1;
    if (y1 >= (int)surface->height) y1 = (int)surface->height - 1;
    if (x0 > x1 || y0 > y1) {
        return;
    }

    int native_x[4];
    int native_y[4];
    gfx_logical_to_native(surface, x0, y0, &native_x[0], &native_y[0]);
    gfx_logical_to_native(surface, x1, y0, &native_x[1], &native_y[1]);
    gfx_logical_to_native(surface, x0, y1, &native_x[2], &native_y[2]);
    gfx_logical_to_native(surface, x1, y1, &native_x[3], &native_y[3]);
    int min_x = native_x[0];
    int max_x = native_x[0];
    int min_y = native_y[0];
    int max_y = native_y[0];
    for (size_t index = 1; index < 4; index++) {
        if (native_x[index] < min_x) min_x = native_x[index];
        if (native_x[index] > max_x) max_x = native_x[index];
        if (native_y[index] < min_y) min_y = native_y[index];
        if (native_y[index] > max_y) max_y = native_y[index];
    }
    for (int tile_y = min_y / GFX_DIRTY_TILE_SIZE;
         tile_y <= max_y / GFX_DIRTY_TILE_SIZE; tile_y++) {
        for (int tile_x = min_x / GFX_DIRTY_TILE_SIZE;
             tile_x <= max_x / GFX_DIRTY_TILE_SIZE; tile_x++) {
            gfx->index8->dirty[(size_t)tile_y * surface->dirty_stride +
                               ((unsigned)tile_x >> 3U)] |=
                (uint8_t)(1U << ((unsigned)tile_x & 7U));
        }
    }
}

static void gfx_mark_index8_all_dirty(solar_os_gfx_t *gfx)
{
    if (gfx_uses_index8(gfx) && gfx->index8->dirty != NULL) {
        memset(gfx->index8->dirty, 0xff, gfx->index8->surface.dirty_size);
    }
}

esp_err_t solar_os_gfx_enable_index8(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (gfx_uses_index8(gfx)) {
        return ESP_OK;
    }
    const u8x8_display_info_t *info = u8g2_GetU8x8(gfx->u8g2)->display_info;
    if (info == NULL || info->pixel_width == 0 || info->pixel_height == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint16_t width = u8g2_GetDisplayWidth(gfx->u8g2);
    const uint16_t height = u8g2_GetDisplayHeight(gfx->u8g2);
    const uint16_t dirty_columns =
        (uint16_t)((info->pixel_width + GFX_DIRTY_TILE_SIZE - 1U) /
                   GFX_DIRTY_TILE_SIZE);
    const uint16_t dirty_rows =
        (uint16_t)((info->pixel_height + GFX_DIRTY_TILE_SIZE - 1U) /
                   GFX_DIRTY_TILE_SIZE);
    const uint16_t dirty_stride = (uint16_t)((dirty_columns + 7U) / 8U);
    const size_t state_bytes =
        (sizeof(solar_os_gfx_index8_surface_t) + sizeof(uint32_t) - 1U) &
        ~(sizeof(uint32_t) - 1U);
    const size_t pixel_bytes = (size_t)width * height;
    const size_t dirty_bytes = (size_t)dirty_stride * dirty_rows;
    const size_t pixel_offset = state_bytes;
    const size_t dirty_offset = pixel_offset + pixel_bytes;
    const size_t palette_offset = dirty_offset + dirty_bytes;
    const size_t palette_aligned = (palette_offset + sizeof(uint16_t) - 1U) &
        ~(sizeof(uint16_t) - 1U);
    const size_t palette_bytes = GFX_INDEX8_PALETTE_SIZE * sizeof(uint16_t);
    const size_t hashes_offset = (palette_aligned + palette_bytes +
        sizeof(uint32_t) - 1U) & ~(sizeof(uint32_t) - 1U);
    const size_t hash_count = (size_t)dirty_columns * dirty_rows;
    const size_t allocation_size = hashes_offset +
        hash_count * sizeof(uint32_t);
    uint8_t *storage = solar_os_memory_calloc(
        1, allocation_size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "gfx.index8");
    if (storage == NULL) {
        return ESP_ERR_NO_MEM;
    }

    solar_os_gfx_index8_surface_t *index8 = (void *)storage;
    index8->pixels = storage + pixel_offset;
    index8->dirty = storage + dirty_offset;
    index8->palette = (uint16_t *)(storage + palette_aligned);
    index8->surface = (solar_os_display_surface_t){
        .data = index8->pixels,
        .data_size = pixel_bytes,
        .palette_rgb565 = index8->palette,
        .palette_size = GFX_INDEX8_PALETTE_SIZE,
        .dirty_tiles = index8->dirty,
        .dirty_size = dirty_bytes,
        .presented_hashes = (uint32_t *)(storage + hashes_offset),
        .presented_hash_count = hash_count,
        .width = width,
        .height = height,
        .stride = width,
        .native_width = info->pixel_width,
        .native_height = info->pixel_height,
        .dirty_stride = dirty_stride,
        .hash_stride = dirty_columns,
        .tile_size = GFX_DIRTY_TILE_SIZE,
        .format = SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .rotation = gfx_rotation(gfx->u8g2),
    };
    gfx->index8 = index8;
    gfx_build_index8_palette(gfx);
    index8->draw_index = gfx_index8_for_color(gfx->color);
    gfx_mark_index8_all_dirty(gfx);
    gfx->dirty = true;
    SOLAR_OS_LOGI(TAG, "INDEX8 surface ready: %ux%u, %u external bytes",
                  (unsigned)width, (unsigned)height,
                  (unsigned)allocation_size);
    return ESP_OK;
}

void solar_os_gfx_prepare_surface(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx) || gfx_uses_index8(gfx)) {
        return;
    }

    char target_name[SOLAR_OS_DISPLAY_TARGET_NAME_MAX] = {0};
    solar_os_display_target_t target;
    if (!solar_os_display_target_name_for_u8g2(
            gfx->u8g2, target_name, sizeof(target_name)) ||
        !solar_os_display_find_target(target_name, &target) ||
        (target.surface_formats & SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT) == 0U) {
        return;
    }
    (void)solar_os_gfx_enable_index8(gfx);
}

void solar_os_gfx_release_surface(solar_os_gfx_t *gfx)
{
    void *storage = solar_os_gfx_detach_surface_storage(gfx);
    if (storage != NULL) {
        solar_os_memory_free(storage);
        SOLAR_OS_LOGI(TAG, "indexed surface released");
    }
}

void *solar_os_gfx_detach_surface_storage(solar_os_gfx_t *gfx)
{
    if (gfx == NULL) {
        return NULL;
    }
    void *storage = gfx->index8;
    gfx->index8 = NULL;
    return storage;
}

const solar_os_display_surface_t *solar_os_gfx_surface(const solar_os_gfx_t *gfx)
{
    return gfx_uses_index8(gfx) ? &gfx->index8->surface : NULL;
}

static bool gfx_valid_rect(int width, int height)
{
    return width > 0 && height > 0;
}

bool solar_os_gfx_display_target_name(const solar_os_gfx_t *gfx,
                                      char *name,
                                      size_t name_len)
{
    return gfx_ready(gfx) &&
        solar_os_display_target_name_for_u8g2(gfx->u8g2, name, name_len);
}

static void gfx_sort_ints(int *values, size_t count)
{
    for (size_t i = 1; i < count; i++) {
        const int value = values[i];
        size_t j = i;
        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            j--;
        }
        values[j] = value;
    }
}

static void gfx_apply_draw_state(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    u8g2_SetDrawColor(gfx->u8g2, gfx_binary_draw_color(gfx, gfx->color));
    u8g2_SetFont(gfx->u8g2, gfx_font_data(gfx->font));
    u8g2_SetFontMode(gfx->u8g2, 1);
    u8g2_SetFontPosBaseline(gfx->u8g2);
}

static void gfx_mark_dirty(solar_os_gfx_t *gfx)
{
    if (gfx != NULL) {
        gfx->dirty = true;
    }
}

static void gfx_draw_hline_raw_clipped(solar_os_gfx_t *gfx, int x, int y, int width);

static void gfx_draw_hline_shade_clipped(solar_os_gfx_t *gfx, int x, int y, int width)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    gfx_draw_hline_raw_clipped(gfx, x, y, width);
}

static void gfx_draw_hline_raw_clipped(solar_os_gfx_t *gfx, int x, int y, int width)
{
    if (!gfx_ready(gfx) || width <= 0) {
        return;
    }

    const int display_width = (int)u8g2_GetDisplayWidth(gfx->u8g2);
    const int display_height = (int)u8g2_GetDisplayHeight(gfx->u8g2);
    if (y < 0 || y >= display_height) {
        return;
    }

    int start = x;
    int end = x + width;
    if (end <= 0 || start >= display_width) {
        return;
    }
    if (start < 0) {
        start = 0;
    }
    if (end > display_width) {
        end = display_width;
    }
    if (end <= start) {
        return;
    }

    if (gfx_uses_index8(gfx)) {
        memset(gfx->index8->pixels +
                   (size_t)y * gfx->index8->surface.stride + start,
               gfx->index8->draw_index,
               (size_t)(end - start));
        gfx_mark_index8_dirty_rect(gfx, start, y, end - start, 1);
        return;
    }

    const uint8_t threshold = gfx_dither_threshold(gfx->color);
    if (threshold == 0 || threshold >= 16) {
        u8g2_SetDrawColor(gfx->u8g2, gfx_apply_polarity(gfx, threshold >= 16 ? 1 : 0));
        u8g2_DrawHLine(gfx->u8g2,
                       (u8g2_uint_t)start,
                       (u8g2_uint_t)y,
                       (u8g2_uint_t)(end - start));
        return;
    }

    int run_start = start;
    uint8_t run_color = gfx_pattern_draw_color(gfx, gfx->color, start, y);
    for (int col = start + 1; col < end; col++) {
        const uint8_t color = gfx_pattern_draw_color(gfx, gfx->color, col, y);
        if (color == run_color) {
            continue;
        }

        u8g2_SetDrawColor(gfx->u8g2, run_color);
        u8g2_DrawHLine(gfx->u8g2,
                       (u8g2_uint_t)run_start,
                       (u8g2_uint_t)y,
                       (u8g2_uint_t)(col - run_start));
        run_start = col;
        run_color = color;
    }

    u8g2_SetDrawColor(gfx->u8g2, run_color);
    u8g2_DrawHLine(gfx->u8g2,
                   (u8g2_uint_t)run_start,
                   (u8g2_uint_t)y,
                   (u8g2_uint_t)(end - run_start));
}

void solar_os_gfx_init(solar_os_gfx_t *gfx, u8g2_t *u8g2)
{
    if (gfx == NULL) {
        return;
    }

    memset(gfx, 0, sizeof(*gfx));
    gfx->u8g2 = u8g2;
    gfx->color = SOLAR_OS_GFX_COLOR_BLACK;
    gfx->font = SOLAR_OS_GFX_FONT_MONO;
    gfx->line_style = SOLAR_OS_GFX_LINE_SOLID;
}

void solar_os_gfx_set_black_is_one(solar_os_gfx_t *gfx, bool black_is_one)
{
    if (gfx == NULL) {
        return;
    }

    gfx->black_is_one = black_is_one;
}

void solar_os_gfx_set_palette_inverted(solar_os_gfx_t *gfx, bool inverted)
{
    if (gfx == NULL) {
        return;
    }
    if (gfx->palette_inverted == inverted) {
        return;
    }

    gfx->palette_inverted = inverted;
    if (gfx_uses_index8(gfx)) {
        gfx_build_index8_palette(gfx);
        gfx_mark_index8_all_dirty(gfx);
    }
    gfx->dirty = true;
}

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx)
{
    return gfx_ready(gfx) ? u8g2_GetDisplayWidth(gfx->u8g2) : 0;
}

size_t solar_os_gfx_height(const solar_os_gfx_t *gfx)
{
    return gfx_ready(gfx) ? u8g2_GetDisplayHeight(gfx->u8g2) : 0;
}

solar_os_display_format_t solar_os_gfx_format(const solar_os_gfx_t *gfx)
{
    return gfx_uses_index8(gfx) ? SOLAR_OS_DISPLAY_FORMAT_INDEX8 :
        SOLAR_OS_DISPLAY_FORMAT_MONO1;
}

bool solar_os_gfx_palette_inverted(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->palette_inverted;
}

solar_os_gfx_color_t solar_os_gfx_gray(uint8_t level)
{
    if (level > SOLAR_OS_GFX_GRAY_MAX) {
        level = SOLAR_OS_GFX_GRAY_MAX;
    }

    return (solar_os_gfx_color_t)(SOLAR_OS_GFX_COLOR_GRAY_BASE + level);
}

solar_os_gfx_color_t solar_os_gfx_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    return SOLAR_OS_GFX_COLOR_RGB_FLAG | ((uint32_t)red << 16U) |
        ((uint32_t)green << 8U) | blue;
}

bool solar_os_gfx_color_is_valid(solar_os_gfx_color_t color)
{
    if (gfx_color_is_gray(color) || gfx_color_is_rgb(color)) {
        return true;
    }

    return color == SOLAR_OS_GFX_COLOR_WHITE ||
        color == SOLAR_OS_GFX_COLOR_LIGHT ||
        color == SOLAR_OS_GFX_COLOR_DARK ||
        color == SOLAR_OS_GFX_COLOR_BLACK;
}

void solar_os_gfx_set_color(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{
    if (gfx == NULL) {
        return;
    }

    gfx->color = color;
    if (gfx_uses_index8(gfx)) {
        gfx->index8->draw_index = gfx_index8_for_color(color);
    }
}

solar_os_gfx_color_t solar_os_gfx_color(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->color : SOLAR_OS_GFX_COLOR_BLACK;
}

void solar_os_gfx_set_font(solar_os_gfx_t *gfx, solar_os_gfx_font_t font)
{
    if (gfx == NULL) {
        return;
    }

    gfx->font = font;
}

solar_os_gfx_font_t solar_os_gfx_font(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->font : SOLAR_OS_GFX_FONT_MONO;
}

size_t solar_os_gfx_text_width(solar_os_gfx_t *gfx, const char *text)
{
    if (!gfx_ready(gfx) || text == NULL) {
        return 0;
    }

    gfx_apply_draw_state(gfx);
    return (size_t)u8g2_GetUTF8Width(gfx->u8g2, text);
}

void solar_os_gfx_set_line_style(solar_os_gfx_t *gfx, solar_os_gfx_line_style_t style)
{
    if (gfx == NULL) {
        return;
    }

    switch (style) {
    case SOLAR_OS_GFX_LINE_SOLID:
    case SOLAR_OS_GFX_LINE_DOTTED:
    case SOLAR_OS_GFX_LINE_DASHED:
        gfx->line_style = style;
        break;
    default:
        break;
    }
}

solar_os_gfx_line_style_t solar_os_gfx_line_style(const solar_os_gfx_t *gfx)
{
    return gfx != NULL ? gfx->line_style : SOLAR_OS_GFX_LINE_SOLID;
}

void solar_os_gfx_clear(solar_os_gfx_t *gfx, solar_os_gfx_color_t color)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    const solar_os_gfx_color_t previous_color = gfx->color;
    gfx->color = color;
    if (gfx_uses_index8(gfx)) {
        const uint8_t index = gfx_index8_for_color(color);
        memset(gfx->index8->pixels, index, gfx->index8->surface.data_size);
        gfx_mark_index8_all_dirty(gfx);
        gfx->color = previous_color;
        gfx->index8->draw_index =
            gfx_index8_for_color(previous_color);
        gfx_mark_dirty(gfx);
        return;
    }
    for (int y = 0; y < (int)u8g2_GetDisplayHeight(gfx->u8g2); y++) {
        gfx_draw_hline_shade_clipped(gfx, 0, y, (int)u8g2_GetDisplayWidth(gfx->u8g2));
    }
    gfx->color = previous_color;
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_pixel(solar_os_gfx_t *gfx, int x, int y)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    gfx_draw_hline_shade_clipped(gfx, x, y, 1);
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_line(solar_os_gfx_t *gfx, int x0, int y0, int x1, int y1)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    unsigned step = 0;

    while (true) {
        bool draw = true;
        switch (gfx->line_style) {
        case SOLAR_OS_GFX_LINE_DOTTED:
            draw = (step % 4U) == 0U;
            break;
        case SOLAR_OS_GFX_LINE_DASHED:
            draw = (step % 12U) < 7U;
            break;
        case SOLAR_OS_GFX_LINE_SOLID:
        default:
            draw = true;
            break;
        }
        if (draw) {
            gfx_draw_hline_shade_clipped(gfx, x0, y0, 1);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        step++;
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height)) {
        return;
    }

    if (gfx_uses_index8(gfx)) {
        gfx_draw_hline_shade_clipped(gfx, x, y, width);
        if (height > 1) {
            gfx_draw_hline_shade_clipped(gfx, x, y + height - 1, width);
        }
        for (int row = y + 1; row < y + height - 1; row++) {
            gfx_draw_hline_shade_clipped(gfx, x, row, 1);
            if (width > 1) {
                gfx_draw_hline_shade_clipped(gfx, x + width - 1, row, 1);
            }
        }
    } else {
        gfx_apply_draw_state(gfx);
        u8g2_DrawFrame(gfx->u8g2,
                       (u8g2_uint_t)x,
                       (u8g2_uint_t)y,
                       (u8g2_uint_t)width,
                       (u8g2_uint_t)height);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_rect(solar_os_gfx_t *gfx, int x, int y, int width, int height)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height)) {
        return;
    }

    for (int row = y; row < y + height; row++) {
        gfx_draw_hline_shade_clipped(gfx, x, row, width);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_polygon(solar_os_gfx_t *gfx,
                               const solar_os_gfx_point_t *points,
                               size_t point_count)
{
    if (!gfx_ready(gfx) || points == NULL || point_count < 3 || point_count > 16) {
        return;
    }

    int min_y = points[0].y;
    int max_y = points[0].y;
    for (size_t i = 1; i < point_count; i++) {
        if (points[i].y < min_y) {
            min_y = points[i].y;
        }
        if (points[i].y > max_y) {
            max_y = points[i].y;
        }
    }

    gfx_apply_draw_state(gfx);
    for (int y = min_y; y <= max_y; y++) {
        int intersections[16];
        size_t intersection_count = 0;
        for (size_t i = 0; i < point_count; i++) {
            const solar_os_gfx_point_t p0 = points[i];
            const solar_os_gfx_point_t p1 = points[(i + 1) % point_count];
            if (p0.y == p1.y) {
                continue;
            }
            if ((y < p0.y && y < p1.y) || (y >= p0.y && y >= p1.y)) {
                continue;
            }

            const int x = p0.x + ((y - p0.y) * (p1.x - p0.x)) / (p1.y - p0.y);
            intersections[intersection_count++] = x;
        }

        if (intersection_count < 2) {
            continue;
        }
        gfx_sort_ints(intersections, intersection_count);
        for (size_t i = 0; i + 1 < intersection_count; i += 2) {
            gfx_draw_hline_shade_clipped(gfx,
                                         intersections[i],
                                         y,
                                         intersections[i + 1] - intersections[i] + 1);
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    if (!gfx_ready(gfx) || radius <= 0) {
        return;
    }

    int dx = radius;
    int dy = 0;
    int err = 0;

    while (dx >= dy) {
        gfx_draw_hline_shade_clipped(gfx, x + dx, y + dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dy, y + dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dy, y + dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dx, y + dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dx, y - dy, 1);
        gfx_draw_hline_shade_clipped(gfx, x - dy, y - dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dy, y - dx, 1);
        gfx_draw_hline_shade_clipped(gfx, x + dx, y - dy, 1);

        dy++;
        if (err <= 0) {
            err += (2 * dy) + 1;
        }
        if (err > 0) {
            dx--;
            err -= (2 * dx) + 1;
        }
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_fill_circle(solar_os_gfx_t *gfx, int x, int y, int radius)
{
    if (!gfx_ready(gfx) || radius <= 0) {
        return;
    }

    gfx_apply_draw_state(gfx);
    const int radius_sq = radius * radius;
    for (int dy = -radius; dy <= radius; dy++) {
        int dx = 0;
        while (((dx + 1) * (dx + 1)) + (dy * dy) <= radius_sq) {
            dx++;
        }
        gfx_draw_hline_shade_clipped(gfx, x - dx, y + dy, (dx * 2) + 1);
    }
    gfx_mark_dirty(gfx);
}

static bool gfx_u8g2_mask_pixel(const solar_os_gfx_t *gfx, int x, int y)
{
    if (!gfx_ready(gfx) || x < 0 || y < 0 ||
        x >= (int)u8g2_GetDisplayWidth(gfx->u8g2) ||
        y >= (int)u8g2_GetDisplayHeight(gfx->u8g2)) {
        return false;
    }
    int native_x = 0;
    int native_y = 0;
    gfx_logical_to_native(&gfx->index8->surface, x, y, &native_x, &native_y);
    const uint16_t row_bytes = (uint16_t)(u8g2_GetBufferTileWidth(gfx->u8g2) * 8U);
    const size_t offset = (size_t)(native_y / 8) * row_bytes + (size_t)native_x;
    return (u8g2_GetBufferPtr(gfx->u8g2)[offset] &
            (uint8_t)(1U << (native_y & 7))) != 0;
}

void solar_os_gfx_text(solar_os_gfx_t *gfx, int x, int baseline_y, const char *text)
{
    if (!gfx_ready(gfx) || text == NULL) {
        return;
    }

    gfx_apply_draw_state(gfx);
    if (gfx_uses_index8(gfx)) {
        const int text_width = (int)u8g2_GetUTF8Width(gfx->u8g2, text);
        const int ascent = (int)u8g2_GetAscent(gfx->u8g2);
        const int descent = (int)u8g2_GetDescent(gfx->u8g2);
        int x0 = x - 2;
        int y0 = baseline_y - ascent - 1;
        int x1 = x + text_width + 2;
        int y1 = baseline_y - descent + 1;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)gfx->index8->surface.width) x1 = gfx->index8->surface.width;
        if (y1 > (int)gfx->index8->surface.height) y1 = gfx->index8->surface.height;
        if (x1 > x0 && y1 > y0) {
            u8g2_SetDrawColor(gfx->u8g2, 0);
            u8g2_DrawBox(gfx->u8g2, (u8g2_uint_t)x0, (u8g2_uint_t)y0,
                         (u8g2_uint_t)(x1 - x0), (u8g2_uint_t)(y1 - y0));
            u8g2_SetDrawColor(gfx->u8g2, 1);
            u8g2_DrawUTF8(gfx->u8g2, (u8g2_uint_t)x,
                          (u8g2_uint_t)baseline_y, text);
            for (int row = y0; row < y1; row++) {
                for (int column = x0; column < x1; column++) {
                    if (gfx_u8g2_mask_pixel(gfx, column, row)) {
                        gfx->index8->pixels[
                            (size_t)row * gfx->index8->surface.stride + column] =
                            gfx->index8->draw_index;
                    }
                }
            }
            gfx_mark_index8_dirty_rect(gfx, x0, y0, x1 - x0, y1 - y0);
        }
    } else {
        u8g2_DrawUTF8(gfx->u8g2, (u8g2_uint_t)x, (u8g2_uint_t)baseline_y, text);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_icon(solar_os_gfx_t *gfx,
                       int x,
                       int y,
                       solar_os_gfx_icon_t icon,
                       solar_os_gfx_icon_size_t size)
{
    const uint8_t *font = gfx_icon_font_data(size);
    if (!gfx_ready(gfx) || font == NULL ||
        (unsigned)icon >= SOLAR_OS_GFX_ICON_COUNT) {
        return;
    }

    const int pixels = (int)size;
    const uint16_t encoding =
        (uint16_t)(GFX_OPEN_ICONIC_FIRST_ENCODING + (unsigned)icon);

    gfx_apply_draw_state(gfx);
    u8g2_SetFont(gfx->u8g2, font);
    u8g2_SetFontPosTop(gfx->u8g2);

    if (gfx_uses_index8(gfx)) {
        int x0 = x;
        int y0 = y;
        int x1 = x + pixels;
        int y1 = y + pixels;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)gfx->index8->surface.width) x1 = gfx->index8->surface.width;
        if (y1 > (int)gfx->index8->surface.height) y1 = gfx->index8->surface.height;
        if (x1 <= x0 || y1 <= y0) {
            return;
        }

        u8g2_SetDrawColor(gfx->u8g2, 0);
        u8g2_DrawBox(gfx->u8g2, (u8g2_uint_t)x0, (u8g2_uint_t)y0,
                     (u8g2_uint_t)(x1 - x0), (u8g2_uint_t)(y1 - y0));
        u8g2_SetDrawColor(gfx->u8g2, 1);
        u8g2_DrawGlyph(gfx->u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, encoding);
        for (int row = y0; row < y1; row++) {
            for (int column = x0; column < x1; column++) {
                if (gfx_u8g2_mask_pixel(gfx, column, row)) {
                    gfx->index8->pixels[
                        (size_t)row * gfx->index8->surface.stride + column] =
                        gfx->index8->draw_index;
                }
            }
        }
        gfx_mark_index8_dirty_rect(gfx, x0, y0, x1 - x0, y1 - y0);
    } else {
        u8g2_DrawGlyph(gfx->u8g2, (u8g2_uint_t)x, (u8g2_uint_t)y, encoding);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_bitmap(solar_os_gfx_t *gfx,
                         int x,
                         int y,
                         int width,
                         int height,
                         const uint8_t *bitmap)
{
    if (!gfx_ready(gfx) || !gfx_valid_rect(width, height) || bitmap == NULL) {
        return;
    }

    if (gfx_uses_index8(gfx)) {
        const size_t stride = ((size_t)width + 7U) / 8U;
        for (int row = 0; row < height; row++) {
            for (int column = 0; column < width; column++) {
                if ((bitmap[(size_t)row * stride + ((unsigned)column >> 3U)] &
                     (uint8_t)(1U << ((unsigned)column & 7U))) != 0) {
                    gfx_draw_hline_raw_clipped(gfx, x + column, y + row, 1);
                }
            }
        }
    } else {
        gfx_apply_draw_state(gfx);
        u8g2_DrawXBM(gfx->u8g2,
                     (u8g2_uint_t)x,
                     (u8g2_uint_t)y,
                     (u8g2_uint_t)width,
                     (u8g2_uint_t)height,
                     bitmap);
    }
    gfx_mark_dirty(gfx);
}

void solar_os_gfx_bitmap_2bpp(solar_os_gfx_t *gfx,
                              int x,
                              int y,
                              int width,
                              int height,
                              const uint8_t *bitmap,
                              size_t bitmap_size,
                              const solar_os_gfx_color_t palette[4])
{
    const size_t pixels = width > 0 && height > 0 ?
        (size_t)width * (size_t)height : 0U;
    const size_t required = (pixels + 3U) / 4U;
    if (!gfx_ready(gfx) || bitmap == NULL || palette == NULL ||
        pixels == 0 || bitmap_size < required) {
        return;
    }

    uint8_t indices[4] = {0};
    if (gfx_uses_index8(gfx)) {
        for (size_t index = 0; index < 4; index++) {
            indices[index] = gfx_index8_for_color(palette[index]);
        }
    }
    for (int row = 0; row < height; row++) {
        const int target_y = y + row;
        if (target_y < 0 || target_y >= (int)solar_os_gfx_height(gfx)) {
            continue;
        }
        for (int column = 0; column < width; column++) {
            const int target_x = x + column;
            if (target_x < 0 || target_x >= (int)solar_os_gfx_width(gfx)) {
                continue;
            }
            const size_t pixel = (size_t)row * (size_t)width + (size_t)column;
            const uint8_t value = (uint8_t)((bitmap[pixel >> 2U] >>
                ((pixel & 3U) * 2U)) & 3U);
            if (gfx_uses_index8(gfx)) {
                gfx->index8->pixels[
                    (size_t)target_y * gfx->index8->surface.stride + target_x] =
                    indices[value];
            } else {
                u8g2_SetDrawColor(gfx->u8g2,
                                  gfx_pattern_draw_color(gfx, palette[value],
                                                         target_x, target_y));
                u8g2_DrawPixel(gfx->u8g2, (u8g2_uint_t)target_x,
                               (u8g2_uint_t)target_y);
            }
        }
    }
    if (gfx_uses_index8(gfx)) {
        gfx_mark_index8_dirty_rect(gfx, x, y, width, height);
    }
    gfx_mark_dirty(gfx);
}

esp_err_t solar_os_gfx_present_mono_xbm(solar_os_gfx_t *gfx,
                                        const uint8_t *bitmap,
                                        size_t bitmap_size,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        size_t stride)
{
    if (!gfx_ready(gfx) ||
        bitmap == NULL ||
        x < 0 ||
        y < 0 ||
        width <= 0 ||
        height <= 0 ||
        stride > UINT16_MAX ||
        x > UINT16_MAX ||
        y > UINT16_MAX ||
        width > UINT16_MAX ||
        height > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t ret = solar_os_display_present_mono_xbm(
        gfx->u8g2,
        bitmap,
        bitmap_size,
        (uint16_t)x,
        (uint16_t)y,
        (uint16_t)width,
        (uint16_t)height,
        (uint16_t)stride,
        gfx->palette_inverted);
    if (ret == ESP_OK) {
        gfx->dirty = false;
    }
    return ret;
}

esp_err_t solar_os_gfx_present_frame(
    solar_os_gfx_t *gfx,
    const solar_os_display_raster_t *frame)
{
    if (!gfx_ready(gfx)) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t ret = solar_os_display_present_frame(gfx->u8g2, frame);
    if (ret == ESP_OK) {
        gfx->dirty = false;
    }
    return ret;
}

bool solar_os_gfx_needs_present(const solar_os_gfx_t *gfx)
{
    return gfx != NULL && gfx->dirty;
}

void solar_os_gfx_present(solar_os_gfx_t *gfx)
{
    if (!gfx_ready(gfx)) {
        return;
    }

    if (gfx_uses_index8(gfx)) {
        gfx_sync_index8_theme(gfx);
        if (solar_os_display_present_surface(
                gfx->u8g2, &gfx->index8->surface) == ESP_OK) {
            memset(gfx->index8->dirty, 0, gfx->index8->surface.dirty_size);
            gfx->dirty = false;
        }
        return;
    }
    solar_os_display_present(gfx->u8g2, SOLAR_OS_DISPLAY_PRESENT_GRAPHICS);
    gfx->dirty = false;
}
