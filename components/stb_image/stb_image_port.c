#include "solar_os_stb_image.h"

#include <stdbool.h>
#include <limits.h>
#include <string.h>

#include "esp_heap_caps.h"

#define SOLAR_OS_STB_GIF_DEFAULT_DELAY_MS 100U
#define SOLAR_OS_STB_GIF_MIN_DELAY_MS 20U

static void *solar_os_stbi_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static void *solar_os_stbi_realloc_sized(void *ptr, size_t old_size, size_t new_size)
{
    if (new_size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    void *new_ptr = solar_os_stbi_malloc(new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    if (new_ptr != NULL && ptr != NULL) {
        memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    }
    heap_caps_free(ptr);
    return new_ptr;
}

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_MALLOC(sz) solar_os_stbi_malloc(sz)
#define STBI_REALLOC_SIZED(p, oldsz, newsz) solar_os_stbi_realloc_sized(p, oldsz, newsz)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static uint16_t solar_os_stbi_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint8_t solar_os_stbi_rgb_to_gray(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint8_t)(((uint32_t)red * 77U +
                      (uint32_t)green * 150U +
                      (uint32_t)blue * 29U) >> 8);
}

static uint32_t solar_os_stbi_normalize_gif_delay(int delay_ms)
{
    if (delay_ms <= 0) {
        return SOLAR_OS_STB_GIF_DEFAULT_DELAY_MS;
    }
    if ((uint32_t)delay_ms < SOLAR_OS_STB_GIF_MIN_DELAY_MS) {
        return SOLAR_OS_STB_GIF_MIN_DELAY_MS;
    }
    return (uint32_t)delay_ms;
}

static uint32_t solar_os_stbi_add_delay(uint32_t current_ms, uint32_t add_ms)
{
    if (UINT32_MAX - current_ms < add_ms) {
        return UINT32_MAX;
    }
    return current_ms + add_ms;
}

static uint32_t solar_os_stbi_div_ceil_u32(uint32_t value, uint32_t divisor)
{
    if (divisor == 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)value + divisor - 1U) / divisor);
}

static void solar_os_stbi_fit_dimensions(uint32_t source_width,
                                         uint32_t source_height,
                                         uint32_t max_output_width,
                                         uint32_t max_output_height,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
    if (out_width == NULL || out_height == NULL) {
        return;
    }

    *out_width = source_width;
    *out_height = source_height;
    if (source_width == 0 || source_height == 0 ||
        max_output_width == 0 || max_output_height == 0 ||
        (source_width <= max_output_width && source_height <= max_output_height)) {
        return;
    }

    const uint64_t height_for_width =
        ((uint64_t)max_output_width * (uint64_t)source_height) / (uint64_t)source_width;
    if (height_for_width <= (uint64_t)max_output_height) {
        *out_width = max_output_width;
        *out_height = height_for_width > 0 ? (uint32_t)height_for_width : 1U;
        return;
    }

    const uint64_t width_for_height =
        ((uint64_t)max_output_height * (uint64_t)source_width) / (uint64_t)source_height;
    *out_width = width_for_height > 0 ? (uint32_t)width_for_height : 1U;
    *out_height = max_output_height;
    if (*out_width == 0) {
        *out_width = 1;
    }
    if (*out_height == 0) {
        *out_height = 1;
    }
}

static uint8_t *solar_os_stbi_load_jpeg_rgb_scaled(const uint8_t *data,
                                                   int len,
                                                   uint32_t output_width,
                                                   uint32_t output_height,
                                                   uint32_t *out_source_width,
                                                   uint32_t *out_source_height)
{
    stbi__context context;
    stbi__start_mem(&context, data, len);

    stbi__jpeg *jpeg = stbi__malloc(sizeof(*jpeg));
    if (jpeg == NULL) {
        stbi__err("outofmem", "Out of memory");
        return NULL;
    }
    memset(jpeg, 0, sizeof(*jpeg));
    jpeg->s = &context;
    stbi__setup_jpeg(jpeg);
    jpeg->s->img_n = 0;

    if (!stbi__decode_jpeg_image(jpeg)) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        return NULL;
    }

    const uint32_t source_width = jpeg->s->img_x;
    const uint32_t source_height = jpeg->s->img_y;
    if (source_width == 0 || source_height == 0 || output_width == 0 ||
        output_height == 0 || output_width > source_width ||
        output_height > source_height) {
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        stbi__err("bad scale", "Invalid JPEG scale");
        return NULL;
    }

    stbi__resample components[4];
    stbi_uc *component_rows[4] = {NULL, NULL, NULL, NULL};
    const int component_count = jpeg->s->img_n;
    for (int component = 0; component < component_count; component++) {
        stbi__resample *resample = &components[component];
        jpeg->img_comp[component].linebuf = stbi__malloc(source_width + 3U);
        if (jpeg->img_comp[component].linebuf == NULL) {
            stbi__cleanup_jpeg(jpeg);
            STBI_FREE(jpeg);
            stbi__err("outofmem", "Out of memory");
            return NULL;
        }

        resample->hs = jpeg->img_h_max / jpeg->img_comp[component].h;
        resample->vs = jpeg->img_v_max / jpeg->img_comp[component].v;
        resample->ystep = resample->vs >> 1;
        resample->w_lores = (source_width + resample->hs - 1) / resample->hs;
        resample->ypos = 0;
        resample->line0 = resample->line1 = jpeg->img_comp[component].data;
        if (resample->hs == 1 && resample->vs == 1) {
            resample->resample = resample_row_1;
        } else if (resample->hs == 1 && resample->vs == 2) {
            resample->resample = stbi__resample_row_v_2;
        } else if (resample->hs == 2 && resample->vs == 1) {
            resample->resample = stbi__resample_row_h_2;
        } else if (resample->hs == 2 && resample->vs == 2) {
            resample->resample = jpeg->resample_row_hv_2_kernel;
        } else {
            resample->resample = stbi__resample_row_generic;
        }
    }

    uint8_t *output = stbi__malloc_mad3(3, output_width, output_height, 0);
    uint8_t *rgba_row = stbi__malloc_mad2(source_width, 4, 0);
    if (output == NULL || rgba_row == NULL) {
        STBI_FREE(output);
        STBI_FREE(rgba_row);
        stbi__cleanup_jpeg(jpeg);
        STBI_FREE(jpeg);
        stbi__err("outofmem", "Out of memory");
        return NULL;
    }

    const bool is_rgb = jpeg->s->img_n == 3 &&
        (jpeg->rgb == 3 ||
         (jpeg->app14_color_transform == 0 && !jpeg->jfif));
    uint32_t output_y = 0;
    for (uint32_t source_y = 0;
         source_y < source_height && output_y < output_height;
         source_y++) {
        for (int component = 0; component < component_count; component++) {
            stbi__resample *resample = &components[component];
            const int bottom = resample->ystep >= (resample->vs >> 1);
            component_rows[component] = resample->resample(
                jpeg->img_comp[component].linebuf,
                bottom ? resample->line1 : resample->line0,
                bottom ? resample->line0 : resample->line1,
                resample->w_lores,
                resample->hs);
            if (++resample->ystep >= resample->vs) {
                resample->ystep = 0;
                resample->line0 = resample->line1;
                if (++resample->ypos < jpeg->img_comp[component].y) {
                    resample->line1 += jpeg->img_comp[component].w2;
                }
            }
        }

        const uint32_t wanted_source_y =
            (uint32_t)(((uint64_t)output_y * source_height) / output_height);
        if (wanted_source_y != source_y) {
            continue;
        }

        if (jpeg->s->img_n == 3 && is_rgb) {
            for (uint32_t x = 0; x < source_width; x++) {
                rgba_row[x * 4U] = component_rows[0][x];
                rgba_row[x * 4U + 1U] = component_rows[1][x];
                rgba_row[x * 4U + 2U] = component_rows[2][x];
                rgba_row[x * 4U + 3U] = 255U;
            }
        } else if (jpeg->s->img_n == 3) {
            jpeg->YCbCr_to_RGB_kernel(rgba_row,
                                      component_rows[0],
                                      component_rows[1],
                                      component_rows[2],
                                      source_width,
                                      4);
        } else if (jpeg->s->img_n == 4 &&
                   jpeg->app14_color_transform == 0) {
            for (uint32_t x = 0; x < source_width; x++) {
                const stbi_uc multiplier = component_rows[3][x];
                rgba_row[x * 4U] =
                    stbi__blinn_8x8(component_rows[0][x], multiplier);
                rgba_row[x * 4U + 1U] =
                    stbi__blinn_8x8(component_rows[1][x], multiplier);
                rgba_row[x * 4U + 2U] =
                    stbi__blinn_8x8(component_rows[2][x], multiplier);
                rgba_row[x * 4U + 3U] = 255U;
            }
        } else if (jpeg->s->img_n == 4) {
            jpeg->YCbCr_to_RGB_kernel(rgba_row,
                                      component_rows[0],
                                      component_rows[1],
                                      component_rows[2],
                                      source_width,
                                      4);
            if (jpeg->app14_color_transform == 2) {
                for (uint32_t x = 0; x < source_width; x++) {
                    const stbi_uc multiplier = component_rows[3][x];
                    rgba_row[x * 4U] =
                        stbi__blinn_8x8(255U - rgba_row[x * 4U], multiplier);
                    rgba_row[x * 4U + 1U] =
                        stbi__blinn_8x8(255U - rgba_row[x * 4U + 1U], multiplier);
                    rgba_row[x * 4U + 2U] =
                        stbi__blinn_8x8(255U - rgba_row[x * 4U + 2U], multiplier);
                }
            }
        } else {
            for (uint32_t x = 0; x < source_width; x++) {
                rgba_row[x * 4U] = component_rows[0][x];
                rgba_row[x * 4U + 1U] = component_rows[0][x];
                rgba_row[x * 4U + 2U] = component_rows[0][x];
                rgba_row[x * 4U + 3U] = 255U;
            }
        }

        uint8_t *target = &output[(size_t)output_y * output_width * 3U];
        for (uint32_t output_x = 0; output_x < output_width; output_x++) {
            const uint32_t source_x =
                (uint32_t)(((uint64_t)output_x * source_width) / output_width);
            target[output_x * 3U] = rgba_row[source_x * 4U];
            target[output_x * 3U + 1U] = rgba_row[source_x * 4U + 1U];
            target[output_x * 3U + 2U] = rgba_row[source_x * 4U + 2U];
        }
        output_y++;
    }

    STBI_FREE(rgba_row);
    stbi__cleanup_jpeg(jpeg);
    STBI_FREE(jpeg);
    if (output_y != output_height) {
        STBI_FREE(output);
        stbi__err("bad scale", "Invalid JPEG scale");
        return NULL;
    }

    *out_source_width = source_width;
    *out_source_height = source_height;
    return output;
}

static void solar_os_stbi_gif_rgba_to_gray_scaled(uint8_t *gray,
                                                  uint32_t output_width,
                                                  uint32_t output_height,
                                                  const uint8_t *rgba,
                                                  uint32_t source_width,
                                                  uint32_t source_height)
{
    if (rgba == NULL || gray == NULL ||
        source_width == 0 || source_height == 0 ||
        output_width == 0 || output_height == 0) {
        return;
    }

    for (uint32_t y = 0; y < output_height; y++) {
        const uint32_t sy = (uint32_t)(((uint64_t)y * source_height) / output_height);
        for (uint32_t x = 0; x < output_width; x++) {
            const uint32_t sx = (uint32_t)(((uint64_t)x * source_width) / output_width);
            const uint8_t *pixel = &rgba[((size_t)sy * source_width + sx) * 4U];
            gray[(size_t)y * output_width + x] =
                pixel[3] < 128U ? 255U : solar_os_stbi_rgb_to_gray(pixel[0], pixel[1], pixel[2]);
        }
    }
}

static void solar_os_stbi_gif_rgba_to_rgb_scaled(uint8_t *rgb,
                                                 uint32_t output_width,
                                                 uint32_t output_height,
                                                 const uint8_t *rgba,
                                                 uint32_t source_width,
                                                 uint32_t source_height)
{
    if (rgba == NULL || rgb == NULL ||
        source_width == 0 || source_height == 0 ||
        output_width == 0 || output_height == 0) {
        return;
    }

    for (uint32_t y = 0; y < output_height; y++) {
        const uint32_t sy = (uint32_t)(((uint64_t)y * source_height) / output_height);
        for (uint32_t x = 0; x < output_width; x++) {
            const uint32_t sx = (uint32_t)(((uint64_t)x * source_width) / output_width);
            const uint8_t *source =
                &rgba[((size_t)sy * source_width + sx) * 4U];
            uint8_t *target = &rgb[((size_t)y * output_width + x) * 3U];
            if (source[3] < 128U) {
                target[0] = 255U;
                target[1] = 255U;
                target[2] = 255U;
            } else {
                target[0] = source[0];
                target[1] = source[1];
                target[2] = source[2];
            }
        }
    }
}

static bool solar_os_stbi_gif_skip_subblocks(const uint8_t *data, size_t len, size_t *offset)
{
    if (data == NULL || offset == NULL) {
        return false;
    }

    while (*offset < len) {
        const uint8_t block_len = data[(*offset)++];
        if (block_len == 0) {
            return true;
        }
        if ((size_t)block_len > len - *offset) {
            return false;
        }
        *offset += block_len;
    }

    return false;
}

static bool solar_os_stbi_gif_color_table_bytes(uint8_t packed, size_t *out_bytes)
{
    if (out_bytes == NULL) {
        return false;
    }
    if ((packed & 0x80U) == 0) {
        *out_bytes = 0;
        return true;
    }

    const size_t entries = (size_t)1U << ((packed & 0x07U) + 1U);
    *out_bytes = entries * 3U;
    return true;
}

static esp_err_t solar_os_stbi_scan_gif(const uint8_t *data,
                                        size_t len,
                                        uint32_t *out_width,
                                        uint32_t *out_height,
                                        uint32_t *out_frames)
{
    if (data == NULL || out_width == NULL || out_height == NULL || out_frames == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < 13U ||
        (memcmp(data, "GIF87a", 6) != 0 && memcmp(data, "GIF89a", 6) != 0)) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t width = solar_os_stbi_read_le16(&data[6]);
    const uint32_t height = solar_os_stbi_read_le16(&data[8]);
    if (width == 0 || height == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t color_table_len = 0;
    if (!solar_os_stbi_gif_color_table_bytes(data[10], &color_table_len) ||
        color_table_len > len - 13U) {
        return ESP_FAIL;
    }

    size_t offset = 13U + color_table_len;
    uint32_t frames = 0;
    while (offset < len) {
        const uint8_t marker = data[offset++];
        if (marker == 0x3b) {
            break;
        }
        if (marker == 0x21) {
            if (offset >= len) {
                return ESP_FAIL;
            }
            offset++;
            if (!solar_os_stbi_gif_skip_subblocks(data, len, &offset)) {
                return ESP_FAIL;
            }
            continue;
        }
        if (marker != 0x2c) {
            return ESP_FAIL;
        }
        if (len - offset < 9U) {
            return ESP_FAIL;
        }
        const uint8_t packed = data[offset + 8U];
        offset += 9U;
        if (!solar_os_stbi_gif_color_table_bytes(packed, &color_table_len) ||
            color_table_len > len - offset) {
            return ESP_FAIL;
        }
        offset += color_table_len;
        if (offset >= len) {
            return ESP_FAIL;
        }
        offset++;
        if (!solar_os_stbi_gif_skip_subblocks(data, len, &offset)) {
            return ESP_FAIL;
        }
        if (frames == UINT32_MAX) {
            return ESP_ERR_INVALID_SIZE;
        }
        frames++;
    }

    if (frames == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    *out_width = width;
    *out_height = height;
    *out_frames = frames;
    return ESP_OK;
}

esp_err_t solar_os_stb_decode_gray(const uint8_t *data,
                                   size_t len,
                                   uint32_t max_pixels,
                                   uint8_t **out_gray,
                                   uint32_t *out_width,
                                   uint32_t *out_height)
{
    if (data == NULL || len == 0 || out_gray == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_gray = NULL;
    *out_width = 0;
    *out_height = 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, (int)len, &width, &height, &channels)) {
        return ESP_FAIL;
    }
    if (width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > SIZE_MAX || (max_pixels != 0 && pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *gray = stbi_load_from_memory(data, (int)len, &width, &height, &channels, 1);
    if (gray == NULL) {
        return ESP_FAIL;
    }

    *out_gray = gray;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    return ESP_OK;
}

esp_err_t solar_os_stb_decode_rgb(const uint8_t *data,
                                  size_t len,
                                  uint32_t max_pixels,
                                  uint8_t **out_rgb,
                                  uint32_t *out_width,
                                  uint32_t *out_height)
{
    if (data == NULL || len == 0 || out_rgb == NULL || out_width == NULL ||
        out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out_rgb = NULL;
    *out_width = 0;
    *out_height = 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, (int)len, &width, &height, &channels) ||
        width <= 0 || height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > SIZE_MAX / 3U || (max_pixels != 0 && pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *rgb = stbi_load_from_memory(data, (int)len, &width, &height,
                                         &channels, 3);
    if (rgb == NULL) {
        return ESP_FAIL;
    }
    *out_rgb = rgb;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    return ESP_OK;
}

esp_err_t solar_os_stb_decode_jpeg_rgb_scaled(const uint8_t *data,
                                              size_t len,
                                              uint32_t max_pixels,
                                              uint32_t max_output_width,
                                              uint32_t max_output_height,
                                              uint8_t **out_rgb,
                                              uint32_t *out_width,
                                              uint32_t *out_height)
{
    if (data == NULL || len == 0 || len > INT_MAX ||
        max_output_width == 0 || max_output_height == 0 ||
        out_rgb == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_rgb = NULL;
    *out_width = 0;
    *out_height = 0;

    int source_width = 0;
    int source_height = 0;
    int channels = 0;
    if (!stbi_info_from_memory(data, (int)len,
                               &source_width, &source_height, &channels) ||
        source_width <= 0 || source_height <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint64_t source_pixels =
        (uint64_t)source_width * (uint64_t)source_height;
    if (source_pixels > SIZE_MAX ||
        (max_pixels != 0 && source_pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t scaled_width = 0;
    uint32_t scaled_height = 0;
    solar_os_stbi_fit_dimensions((uint32_t)source_width,
                                 (uint32_t)source_height,
                                 max_output_width,
                                 max_output_height,
                                 &scaled_width,
                                 &scaled_height);
    if (scaled_width == 0 || scaled_height == 0 ||
        (uint64_t)scaled_width * scaled_height > SIZE_MAX / 3U) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t decoded_source_width = 0;
    uint32_t decoded_source_height = 0;
    uint8_t *rgb = solar_os_stbi_load_jpeg_rgb_scaled(
        data,
        (int)len,
        scaled_width,
        scaled_height,
        &decoded_source_width,
        &decoded_source_height);
    if (rgb == NULL) {
        const char *reason = stbi_failure_reason();
        return reason != NULL && strcmp(reason, "outofmem") == 0 ?
            ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (decoded_source_width != (uint32_t)source_width ||
        decoded_source_height != (uint32_t)source_height) {
        STBI_FREE(rgb);
        return ESP_FAIL;
    }

    *out_rgb = rgb;
    *out_width = scaled_width;
    *out_height = scaled_height;
    return ESP_OK;
}

static esp_err_t solar_os_stb_decode_gif(const uint8_t *data,
                                         size_t len,
                                         uint32_t max_frame_pixels,
                                         uint32_t max_total_pixels,
                                         uint32_t max_output_width,
                                         uint32_t max_output_height,
                                         uint8_t channels,
                                         solar_os_stb_rgba_to_gray_scaled_fn gray_converter,
                                         solar_os_stb_rgba_to_rgb_scaled_fn rgb_converter,
                                         solar_os_stb_gif_animation_t *out_animation)
{
    if (data == NULL || len == 0 || out_animation == NULL ||
        (channels != 1U && channels != 3U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out_animation, 0, sizeof(*out_animation));

    uint32_t scan_width = 0;
    uint32_t scan_height = 0;
    uint32_t scan_frames = 0;
    esp_err_t err = solar_os_stbi_scan_gif(data, len, &scan_width, &scan_height, &scan_frames);
    if (err != ESP_OK) {
        return err;
    }

    const uint64_t canvas_pixels = (uint64_t)scan_width * (uint64_t)scan_height;
    if (canvas_pixels > SIZE_MAX ||
        (max_frame_pixels != 0 && canvas_pixels > max_frame_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t output_width = 0;
    uint32_t output_height = 0;
    solar_os_stbi_fit_dimensions(scan_width,
                                 scan_height,
                                 max_output_width,
                                 max_output_height,
                                 &output_width,
                                 &output_height);

    const uint64_t output_pixels = (uint64_t)output_width * (uint64_t)output_height;
    if (output_pixels == 0 || output_pixels > SIZE_MAX / channels ||
        (max_total_pixels != 0 && output_pixels > max_total_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t max_stored_frames = scan_frames;
    if (max_total_pixels != 0) {
        max_stored_frames = (uint32_t)(max_total_pixels / output_pixels);
        if (max_stored_frames == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        if (max_stored_frames > scan_frames) {
            max_stored_frames = scan_frames;
        }
    }

    const uint32_t store_step = solar_os_stbi_div_ceil_u32(scan_frames, max_stored_frames);
    const uint32_t stored_capacity = solar_os_stbi_div_ceil_u32(scan_frames, store_step);
    const uint64_t stored_pixels = output_pixels * stored_capacity;
    if (store_step == 0 || stored_capacity == 0 ||
        stored_pixels == 0 || stored_pixels > SIZE_MAX / channels ||
        (max_total_pixels != 0 && stored_pixels > max_total_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t stored_bytes = (size_t)stored_pixels * channels;
    uint8_t *pixels = solar_os_stbi_malloc(stored_bytes);
    uint32_t *delays_ms = solar_os_stbi_malloc((size_t)stored_capacity * sizeof(uint32_t));
    stbi__gif *gif = (stbi__gif *)stbi__malloc(sizeof(*gif));
    if (pixels == NULL || delays_ms == NULL || gif == NULL) {
        STBI_FREE(pixels);
        STBI_FREE(delays_ms);
        STBI_FREE(gif);
        return ESP_ERR_NO_MEM;
    }
    memset(gif, 0, sizeof(*gif));

    stbi__context context;
    stbi__start_mem(&context, data, (int)len);
    if (!stbi__gif_test(&context)) {
        STBI_FREE(pixels);
        STBI_FREE(delays_ms);
        STBI_FREE(gif);
        return ESP_ERR_INVALID_ARG;
    }

    int components = 0;
    uint32_t decoded_frames = 0;
    uint32_t stored_frames = 0;
    uint32_t pending_delay_ms = 0;
    for (;;) {
        stbi_uc *rgba = stbi__gif_load_next(&context, gif, &components, 4, NULL);
        if (rgba == (stbi_uc *)&context) {
            break;
        }
        if (rgba == NULL) {
            err = ESP_FAIL;
            break;
        }
        if (gif->w <= 0 || gif->h <= 0 ||
            (uint32_t)gif->w != scan_width ||
            (uint32_t)gif->h != scan_height) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        const uint32_t delay_ms = solar_os_stbi_normalize_gif_delay(gif->delay);
        if ((decoded_frames % store_step) == 0) {
            if (stored_frames > 0) {
                delays_ms[stored_frames - 1U] =
                    pending_delay_ms != 0 ? pending_delay_ms : SOLAR_OS_STB_GIF_DEFAULT_DELAY_MS;
                pending_delay_ms = 0;
            }
            if (stored_frames >= stored_capacity) {
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            uint8_t *frame = pixels +
                ((size_t)stored_frames * (size_t)output_pixels * channels);
            if (channels == 3U && rgb_converter != NULL) {
                rgb_converter(frame,
                              output_width,
                              output_height,
                              rgba,
                              scan_width,
                              scan_height);
            } else if (channels == 3U) {
                solar_os_stbi_gif_rgba_to_rgb_scaled(frame,
                                                     output_width,
                                                     output_height,
                                                     rgba,
                                                     scan_width,
                                                     scan_height);
            } else if (gray_converter != NULL) {
                gray_converter(frame,
                               output_width,
                               output_height,
                               rgba,
                               scan_width,
                               scan_height);
            } else {
                solar_os_stbi_gif_rgba_to_gray_scaled(frame,
                                                      output_width,
                                                      output_height,
                                                      rgba,
                                                      scan_width,
                                                      scan_height);
            }
            stored_frames++;
        }

        pending_delay_ms = solar_os_stbi_add_delay(pending_delay_ms, delay_ms);
        decoded_frames++;
    }

    STBI_FREE(gif->out);
    STBI_FREE(gif->history);
    STBI_FREE(gif->background);
    STBI_FREE(gif);

    if (err != ESP_OK || decoded_frames == 0 || stored_frames == 0) {
        STBI_FREE(pixels);
        STBI_FREE(delays_ms);
        return err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
    }

    delays_ms[stored_frames - 1U] =
        pending_delay_ms != 0 ? pending_delay_ms : SOLAR_OS_STB_GIF_DEFAULT_DELAY_MS;

    if (stored_frames < stored_capacity) {
        uint8_t *trimmed_pixels = solar_os_stbi_realloc_sized(
            pixels,
            stored_bytes,
            (size_t)stored_frames * (size_t)output_pixels * channels);
        if (trimmed_pixels != NULL) {
            pixels = trimmed_pixels;
        }
        uint32_t *trimmed_delays = solar_os_stbi_realloc_sized(delays_ms,
                                                               (size_t)stored_capacity *
                                                                   sizeof(uint32_t),
                                                               (size_t)stored_frames *
                                                                   sizeof(uint32_t));
        if (trimmed_delays != NULL) {
            delays_ms = trimmed_delays;
        }
    }

    out_animation->width = output_width;
    out_animation->height = output_height;
    out_animation->frame_count = stored_frames;
    out_animation->pixels = pixels;
    out_animation->channels = channels;
    out_animation->delays_ms = delays_ms;
    return ESP_OK;
}

esp_err_t solar_os_stb_decode_gif_gray(const uint8_t *data,
                                       size_t len,
                                       uint32_t max_frame_pixels,
                                       uint32_t max_total_pixels,
                                       uint32_t max_output_width,
                                       uint32_t max_output_height,
                                       solar_os_stb_rgba_to_gray_scaled_fn converter,
                                       solar_os_stb_gif_animation_t *out_animation)
{
    return solar_os_stb_decode_gif(data,
                                   len,
                                   max_frame_pixels,
                                   max_total_pixels,
                                   max_output_width,
                                   max_output_height,
                                   1U,
                                   converter,
                                   NULL,
                                   out_animation);
}

esp_err_t solar_os_stb_decode_gif_rgb(const uint8_t *data,
                                      size_t len,
                                      uint32_t max_frame_pixels,
                                      uint32_t max_total_pixels,
                                      uint32_t max_output_width,
                                      uint32_t max_output_height,
                                      solar_os_stb_rgba_to_rgb_scaled_fn converter,
                                      solar_os_stb_gif_animation_t *out_animation)
{
    return solar_os_stb_decode_gif(data,
                                   len,
                                   max_frame_pixels,
                                   max_total_pixels,
                                   max_output_width,
                                   max_output_height,
                                   3U,
                                   NULL,
                                   converter,
                                   out_animation);
}

esp_err_t solar_os_stb_jpeg_decode_gray(const uint8_t *data,
                                         size_t len,
                                         uint32_t max_pixels,
                                         uint8_t **out_gray,
                                         uint32_t *out_width,
                                         uint32_t *out_height)
{
    return solar_os_stb_decode_gray(data,
                                    len,
                                    max_pixels,
                                    out_gray,
                                    out_width,
                                    out_height);
}

void solar_os_stb_gif_animation_free(solar_os_stb_gif_animation_t *animation)
{
    if (animation == NULL) {
        return;
    }

    stbi_image_free(animation->pixels);
    STBI_FREE(animation->delays_ms);
    memset(animation, 0, sizeof(*animation));
}

void solar_os_stb_image_free(void *data)
{
    stbi_image_free(data);
}

const char *solar_os_stb_failure_reason(void)
{
    const char *reason = stbi_failure_reason();
    return reason != NULL ? reason : "unknown";
}
