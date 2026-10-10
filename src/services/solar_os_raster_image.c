#include "solar_os_raster_image.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_rgb565.h"
#include "solar_os_stb_image.h"
#include "solar_os_webp_decoder.h"

#define RASTER_IMAGE_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define RASTER_IMAGE_MAX_PIXELS (2U * 1024U * 1024U)

typedef enum {
    RASTER_IMAGE_PIXELS_STB,
    RASTER_IMAGE_PIXELS_WEBP,
    RASTER_IMAGE_PIXELS_NATIVE,
} raster_image_pixels_owner_t;

struct solar_os_raster_image {
    uint32_t references;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
    uint8_t *rgb565;
    raster_image_pixels_owner_t pixels_owner;
};

static bool raster_image_is_webp(const uint8_t *data, size_t len)
{
    return data != NULL && len >= 12U &&
        memcmp(data, "RIFF", 4U) == 0 &&
        memcmp(data + 8U, "WEBP", 4U) == 0;
}

static esp_err_t raster_image_read_file(const char *path,
                                        uint8_t **out_data,
                                        size_t *out_len)
{
    if (path == NULL || path[0] == '\0' || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_data = NULL;
    *out_len = 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0 || (uint64_t)st.st_size > RASTER_IMAGE_MAX_FILE_BYTES ||
        (uint64_t)st.st_size > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    const size_t len = (size_t)st.st_size;
    uint8_t *data = solar_os_memory_alloc(len,
                                          SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                          "raster.file");
    if (data == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    const bool read_ok = fread(data, 1U, len, file) == len;
    fclose(file);
    if (!read_ok) {
        solar_os_memory_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = len;
    return ESP_OK;
}

esp_err_t solar_os_raster_image_open(const char *path,
                                     solar_os_raster_image_t **out_image)
{
    if (out_image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_image = NULL;

    uint8_t *data = NULL;
    size_t data_len = 0;
    esp_err_t err = raster_image_read_file(path, &data, &data_len);
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_raster_image_decode(data, data_len, out_image);
    solar_os_memory_free(data);
    return err;
}

esp_err_t solar_os_raster_image_decode(const uint8_t *data, size_t data_len,
                                       solar_os_raster_image_t **out_image)
{
    if (out_image == NULL) return ESP_ERR_INVALID_ARG;
    *out_image = NULL;
    if (data == NULL || data_len == 0U || data_len > RASTER_IMAGE_MAX_FILE_BYTES)
        return ESP_ERR_INVALID_SIZE;
    esp_err_t err;
    solar_os_raster_image_t *image = solar_os_memory_calloc(
        1U,
        sizeof(*image),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "raster.image");
    if (image == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (raster_image_is_webp(data, data_len)) {
        err = solar_os_webp_decode_rgb(data,
                                       data_len,
                                       RASTER_IMAGE_MAX_PIXELS,
                                       &image->pixels,
                                       &image->width,
                                       &image->height);
        image->pixels_owner = RASTER_IMAGE_PIXELS_WEBP;
    } else {
        err = solar_os_stb_decode_rgb(data,
                                      data_len,
                                      RASTER_IMAGE_MAX_PIXELS,
                                      &image->pixels,
                                      &image->width,
                                      &image->height);
        image->pixels_owner = RASTER_IMAGE_PIXELS_STB;
    }
    if (err != ESP_OK) {
        solar_os_memory_free(image);
        return err;
    }

    image->references = 1U;
    *out_image = image;
    return ESP_OK;
}

void solar_os_raster_image_retain(solar_os_raster_image_t *image)
{
    if (image != NULL) {
        (void)__atomic_add_fetch(&image->references, 1U, __ATOMIC_RELAXED);
    }
}

esp_err_t solar_os_raster_image_from_pixels(const uint8_t *data, size_t length,
    uint32_t width, uint32_t height, solar_os_raster_image_format_t format,
    size_t stride, solar_os_raster_image_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!data || !width || !height ||
        (uint64_t)width * height > RASTER_IMAGE_MAX_PIXELS)
        return ESP_ERR_INVALID_SIZE;
    const size_t channels = format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? 1 :
        format == SOLAR_OS_RASTER_IMAGE_RGB565_LE ? 2 :
        format == SOLAR_OS_RASTER_IMAGE_RGB888 ? 3 : 0;
    if (!channels) return ESP_ERR_INVALID_ARG;
    const size_t row = (size_t)width * channels;
    if (!stride) stride = row;
    if (stride < row || (height > 1 && stride > (SIZE_MAX - row) / (height - 1)) ||
        length < stride * (height - 1) + row) return ESP_ERR_INVALID_SIZE;
    solar_os_raster_image_t *image = solar_os_memory_calloc(1, sizeof(*image),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "raster.image");
    if (!image) return ESP_ERR_NO_MEM;
    image->pixels = solar_os_memory_alloc((size_t)width * height * 3,
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "raster.pixels");
    if (!image->pixels) { solar_os_memory_free(image); return ESP_ERR_NO_MEM; }
    image->width = width; image->height = height; image->references = 1;
    image->pixels_owner = RASTER_IMAGE_PIXELS_NATIVE;
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *src = data + y * stride;
        uint8_t *dst = image->pixels + (size_t)y * width * 3;
        if (channels == 3) { memcpy(dst, src, row); continue; }
        for (uint32_t x = 0; x < width; ++x) {
            if (channels == 1) dst[x*3] = dst[x*3+1] = dst[x*3+2] = src[x];
            else {
                uint16_t p = src[x*2] | ((uint16_t)src[x*2+1] << 8);
                unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
                dst[x*3] = (r << 3) | (r >> 2);
                dst[x*3+1] = (g << 2) | (g >> 4);
                dst[x*3+2] = (b << 3) | (b >> 2);
            }
        }
    }
    *out = image;
    return ESP_OK;
}

void solar_os_raster_image_release(solar_os_raster_image_t *image)
{
    if (image == NULL ||
        __atomic_sub_fetch(&image->references, 1U, __ATOMIC_ACQ_REL) != 0U) {
        return;
    }

    if (image->pixels_owner == RASTER_IMAGE_PIXELS_NATIVE) {
        solar_os_memory_free(image->pixels);
    } else if (image->pixels_owner == RASTER_IMAGE_PIXELS_WEBP) {
        solar_os_webp_free(image->pixels);
    } else {
        solar_os_stb_image_free(image->pixels);
    }
    image->pixels = NULL;
    solar_os_memory_free(image->rgb565);
    solar_os_memory_free(image);
}

uint32_t solar_os_raster_image_width(const solar_os_raster_image_t *image)
{
    return image != NULL ? image->width : 0U;
}

uint32_t solar_os_raster_image_height(const solar_os_raster_image_t *image)
{
    return image != NULL ? image->height : 0U;
}

esp_err_t solar_os_raster_image_pixels(const solar_os_raster_image_t *image,
                                      solar_os_raster_image_pixels_t *pixels)
{
    if (!pixels) return ESP_ERR_INVALID_ARG;
    *pixels = (solar_os_raster_image_pixels_t){0};
    if (!image || !image->pixels) return ESP_ERR_INVALID_ARG;
    *pixels = (solar_os_raster_image_pixels_t){
        .data = image->pixels, .length = (size_t)image->width * image->height * 3U,
        .stride = (size_t)image->width * 3U,
        .width = image->width, .height = image->height,
    };
    return ESP_OK;
}

esp_err_t solar_os_raster_image_convert(const solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    uint8_t *destination, size_t length, size_t stride)
{
    if (!image || !image->pixels || !options || !destination)
        return ESP_ERR_INVALID_ARG;
    const uint32_t x = options->x, y = options->y;
    if (x >= image->width || y >= image->height) return ESP_ERR_INVALID_SIZE;
    const uint32_t cw = options->width ? options->width : image->width - x;
    const uint32_t ch = options->height ? options->height : image->height - y;
    const uint32_t w = options->output_width ? options->output_width : cw;
    const uint32_t h = options->output_height ? options->output_height : ch;
    if (cw > image->width - x || ch > image->height - y ||
        !w || !h || (uint64_t)w * h > RASTER_IMAGE_MAX_PIXELS)
        return ESP_ERR_INVALID_SIZE;
    size_t bytes;
    switch (options->format) {
    case SOLAR_OS_RASTER_IMAGE_GRAY8: bytes = 1; break;
    case SOLAR_OS_RASTER_IMAGE_RGB565_LE: bytes = 2; break;
    case SOLAR_OS_RASTER_IMAGE_RGB888: bytes = 3; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    const size_t row_bytes = (size_t)w * bytes;
    if (!stride) stride = row_bytes;
    if (stride < row_bytes || row_bytes > length ||
        (h > 1 && stride > (length - row_bytes) / (h - 1U)))
        return ESP_ERR_INVALID_SIZE;
    const size_t used = (size_t)(h - 1U) * stride + row_bytes;
    const uintptr_t dst_start = (uintptr_t)destination;
    const uintptr_t src_start = (uintptr_t)image->pixels;
    const size_t src_length = (size_t)image->width * image->height * 3U;
    if (used > UINTPTR_MAX - dst_start) return ESP_ERR_INVALID_SIZE;
    if (dst_start < src_start + src_length && src_start < dst_start + used)
        return ESP_ERR_INVALID_ARG;
    for (uint32_t row = 0; row < h; ++row) {
        const uint32_t sy = y + (uint64_t)row * ch / h;
        uint8_t *dst = destination + (size_t)row * stride;
        for (uint32_t col = 0; col < w; ++col) {
            const uint32_t sx = x + (uint64_t)col * cw / w;
            const uint8_t *rgb = image->pixels + ((size_t)sy * image->width + sx) * 3U;
            if (options->format == SOLAR_OS_RASTER_IMAGE_GRAY8) {
                *dst++ = ((uint32_t)rgb[0] * 77U + rgb[1] * 150U + rgb[2] * 29U) >> 8;
            } else if (options->format == SOLAR_OS_RASTER_IMAGE_RGB565_LE) {
                const uint16_t value = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
                    ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
                *dst++ = value; *dst++ = value >> 8U;
            } else {
                memcpy(dst, rgb, 3U); dst += 3U;
            }
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_raster_image_draw(const solar_os_raster_image_t *image,
                                     solar_os_gfx_t *gfx,
                                     int x,
                                     int y,
                                     uint32_t width,
                                     uint32_t height)
{
    if (image == NULL || gfx == NULL || image->pixels == NULL ||
        image->width == 0U || image->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (width == 0U) {
        width = image->width;
    }
    if (height == 0U) {
        height = image->height;
    }
    if (width > INT_MAX || height > INT_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    const solar_os_gfx_raster_t raster = {
        .pixels = image->pixels,
        .pixels_size = (size_t)image->width * image->height * 3U,
        .width = image->width,
        .height = image->height,
        .stride = (size_t)image->width * 3U,
        .format = SOLAR_OS_GFX_RASTER_RGB888,
    };
    return solar_os_gfx_blit_raster(gfx,
                                    &raster,
                                    x,
                                    y,
                                    (int)width,
                                    (int)height,
                                    NULL);
}

esp_err_t solar_os_raster_image_present(solar_os_raster_image_t *image,
    solar_os_gfx_t *gfx, int x, int y, uint32_t width, uint32_t height)
{
    if (!image || !gfx || !image->pixels || x < 0 || y < 0) return ESP_ERR_INVALID_ARG;
    if (!width) width = image->width;
    if (!height) height = image->height;
    if (!width || !height || (uint64_t)x + width > (uint32_t)solar_os_gfx_width(gfx) ||
        (uint64_t)y + height > (uint32_t)solar_os_gfx_height(gfx)) return ESP_ERR_INVALID_SIZE;
    if (!solar_os_gfx_supports_frame_format(gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565)) {
        esp_err_t err = solar_os_raster_image_draw(image, gfx, x, y, width, height);
        if (err == ESP_OK) solar_os_gfx_present(gfx);
        return err;
    }
    if (image->width > UINT16_MAX / 2U || image->height > UINT16_MAX ||
        width > UINT16_MAX || height > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    const size_t count = (size_t)image->width * image->height;
    if (!image->rgb565) {
        image->rgb565 = solar_os_memory_alloc(count * 2U,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "raster.rgb565");
        if (!image->rgb565) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *rgb = image->pixels + i * 3U;
            const uint16_t color = ((uint16_t)(rgb[0] & 0xf8U) << 8U) |
                ((uint16_t)(rgb[1] & 0xfcU) << 3U) | (rgb[2] >> 3U);
            image->rgb565[i * 2U] = color >> 8U;
            image->rgb565[i * 2U + 1U] = color;
        }
    }
    const solar_os_display_raster_t raster = {
        .data = image->rgb565, .data_size = count * 2U,
        .source_width = image->width, .source_height = image->height,
        .source_stride = image->width * 2U, .x = x, .y = y,
        .width = width, .height = height, .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
    };
    return solar_os_gfx_present_frame(gfx, &raster);
}
