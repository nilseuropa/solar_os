#include "solar_os_webp_decoder.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "webp/decode.h"

static void *webp_alloc(size_t len)
{
    void *ptr = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    return ptr;
}

esp_err_t solar_os_webp_decode_gray(const uint8_t *data,
                                    size_t len,
                                    uint32_t max_pixels,
                                    uint8_t **out_gray,
                                    uint32_t *out_width,
                                    uint32_t *out_height)
{
    if (data == NULL || len == 0 || out_gray == NULL || out_width == NULL || out_height == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_gray = NULL;
    *out_width = 0;
    *out_height = 0;

    int width = 0;
    int height = 0;
    if (!WebPGetInfo(data, len, &width, &height) || width <= 0 || height <= 0) {
        return ESP_FAIL;
    }

    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > SIZE_MAX || pixels > INT_MAX || (max_pixels != 0 && pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const int uv_width = (width + 1) / 2;
    const int uv_height = (height + 1) / 2;
    const size_t gray_len = (size_t)pixels;
    const size_t uv_len = (size_t)uv_width * (size_t)uv_height;

    uint8_t *gray = webp_alloc(gray_len);
    uint8_t *u = webp_alloc(uv_len);
    uint8_t *v = webp_alloc(uv_len);
    if (gray == NULL || u == NULL || v == NULL) {
        heap_caps_free(gray);
        heap_caps_free(u);
        heap_caps_free(v);
        return ESP_ERR_NO_MEM;
    }

    uint8_t *decoded = WebPDecodeYUVInto(data,
                                         len,
                                         gray,
                                         gray_len,
                                         width,
                                         u,
                                         uv_len,
                                         uv_width,
                                         v,
                                         uv_len,
                                         uv_width);
    heap_caps_free(u);
    heap_caps_free(v);
    if (decoded != gray) {
        heap_caps_free(gray);
        return ESP_FAIL;
    }

    *out_gray = gray;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    return ESP_OK;
}

esp_err_t solar_os_webp_decode_rgb(const uint8_t *data,
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

    *out_rgb = NULL;
    *out_width = 0;
    *out_height = 0;

    int width = 0;
    int height = 0;
    if (!WebPGetInfo(data, len, &width, &height) || width <= 0 || height <= 0) {
        return ESP_FAIL;
    }

    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > SIZE_MAX / 3U || pixels > INT_MAX ||
        (max_pixels != 0 && pixels > max_pixels)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const size_t rgb_len = (size_t)pixels * 3U;
    uint8_t *rgb = webp_alloc(rgb_len);
    if (rgb == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (WebPDecodeRGBInto(data, len, rgb, rgb_len, width * 3) != rgb) {
        heap_caps_free(rgb);
        return ESP_FAIL;
    }

    *out_rgb = rgb;
    *out_width = (uint32_t)width;
    *out_height = (uint32_t)height;
    return ESP_OK;
}

void solar_os_webp_free(void *data)
{
    heap_caps_free(data);
}
