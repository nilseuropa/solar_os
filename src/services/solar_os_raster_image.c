#include "solar_os_raster_image.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_gfx.h"
#include "solar_os_memory.h"
#include "solar_os_stb_image.h"
#include "solar_os_webp_decoder.h"

#define RASTER_IMAGE_MAX_FILE_BYTES (4U * 1024U * 1024U)
#define RASTER_IMAGE_MAX_PIXELS (2U * 1024U * 1024U)

typedef enum {
    RASTER_IMAGE_PIXELS_STB,
    RASTER_IMAGE_PIXELS_WEBP,
} raster_image_pixels_owner_t;

struct solar_os_raster_image {
    uint32_t references;
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
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

    solar_os_raster_image_t *image = solar_os_memory_calloc(
        1U,
        sizeof(*image),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
        "raster.image");
    if (image == NULL) {
        solar_os_memory_free(data);
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
    solar_os_memory_free(data);
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

void solar_os_raster_image_release(solar_os_raster_image_t *image)
{
    if (image == NULL ||
        __atomic_sub_fetch(&image->references, 1U, __ATOMIC_ACQ_REL) != 0U) {
        return;
    }

    if (image->pixels_owner == RASTER_IMAGE_PIXELS_WEBP) {
        solar_os_webp_free(image->pixels);
    } else {
        solar_os_stb_image_free(image->pixels);
    }
    image->pixels = NULL;
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
