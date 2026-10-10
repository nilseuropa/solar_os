#include "solar_os_tensor_image.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <float.h>
#include <math.h>
#include <string.h>

void solar_os_tensor_image_defaults(solar_os_tensor_image_options_t *o)
{
    *o = (solar_os_tensor_image_options_t){.std = {1, 1, 1}};
}
esp_err_t solar_os_tensor_image_option(solar_os_tensor_image_options_t *o,
    const char *key, const char *value)
{
    if (!o || !key || !value) return ESP_ERR_INVALID_ARG;
    if (!strcmp(key, "layout")) {
        const char *names[] = {"NHWC", "NCHW", "HWC", "CHW"};
        for (unsigned i = 0; i < 4; ++i) if (!strcmp(value, names[i])) {
            o->layout = i; return ESP_OK;
        }
    } else if (!strcmp(key, "color")) {
        const char *names[] = {"RGB", "BGR", "GRAY"};
        for (unsigned i = 0; i < 3; ++i) if (!strcmp(value, names[i])) {
            o->color = i; return ESP_OK;
        }
    } else if (!strcmp(key, "resize")) {
        if (!strcmp(value, "stretch")) { o->resize = SOLAR_OS_TENSOR_STRETCH; return ESP_OK; }
        if (!strcmp(value, "letterbox")) { o->resize = SOLAR_OS_TENSOR_LETTERBOX; return ESP_OK; }
    }
    return ESP_ERR_INVALID_ARG;
}
esp_err_t solar_os_tensor_image_inspect(const solar_os_raster_image_pixels_t *s,
    const solar_os_inference_tensor_t *p, const solar_os_tensor_image_options_t *o,
    solar_os_tensor_image_transform_t *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    *t = (solar_os_tensor_image_transform_t){0};
    if (!s || !p || !o || !s->data || !s->width || !s->height ||
        (unsigned)o->layout > SOLAR_OS_TENSOR_CHW || (unsigned)o->color > SOLAR_OS_TENSOR_GRAY ||
        (unsigned)o->resize > SOLAR_OS_TENSOR_LETTERBOX) return ESP_ERR_INVALID_ARG;
    if ((uint64_t)s->width * s->height > 2U * 1024U * 1024U ||
        s->stride < (uint64_t)s->width * 3 || s->stride > SIZE_MAX / s->height ||
        s->length < (s->height - 1U) * s->stride + (size_t)s->width * 3)
        return ESP_ERR_INVALID_SIZE;
    const bool batch = o->layout == SOLAR_OS_TENSOR_NHWC || o->layout == SOLAR_OS_TENSOR_NCHW;
    const bool planar = o->layout == SOLAR_OS_TENSOR_NCHW || o->layout == SOLAR_OS_TENSOR_CHW;
    if (p->rank != (batch ? 4U : 3U) || (batch && p->shape[0] != 1)) return ESP_ERR_INVALID_SIZE;
    const uint32_t *shape = p->shape + (batch ? 1 : 0);
    const uint32_t channels = shape[planar ? 0 : 2];
    const uint32_t width = shape[planar ? 2 : 1], height = shape[planar ? 1 : 0];
    if (channels != (o->color == SOLAR_OS_TENSOR_GRAY ? 1U : 3U) || !width || !height ||
        (uint64_t)width * height > 2U * 1024U * 1024U) return ESP_ERR_INVALID_SIZE;
    size_t element;
    switch (p->dtype) {
    case SOLAR_OS_TENSOR_INT8: case SOLAR_OS_TENSOR_UINT8: element = 1; break;
    case SOLAR_OS_TENSOR_INT16: element = 2; break;
    case SOLAR_OS_TENSOR_FLOAT32: element = 4; break;
    default: return ESP_ERR_NOT_SUPPORTED;
    }
    const uint64_t bytes = (uint64_t)width * height * channels * element;
    if (bytes > SOLAR_OS_INFERENCE_TENSOR_MAX || bytes != p->bytes) return ESP_ERR_INVALID_SIZE;
    if (p->dtype == SOLAR_OS_TENSOR_FLOAT32) {
        if (p->exponent_count) return ESP_ERR_INVALID_ARG;
    } else {
        if (!p->exponents || (p->exponent_count != 1 && p->exponent_count != channels))
            return ESP_ERR_INVALID_ARG;
        for (size_t c = 0; c < p->exponent_count; ++c)
            if (p->exponents[c] < -126 || p->exponents[c] > 126) return ESP_ERR_INVALID_ARG;
    }
    for (unsigned c = 0; c < channels; ++c)
        if (!isfinite(o->mean[c]) || !isfinite(o->std[c]) || o->std[c] <= 0)
            return ESP_ERR_INVALID_ARG;
    if (o->x >= s->width || o->y >= s->height) return ESP_ERR_INVALID_SIZE;
    const uint32_t cw = o->width ? o->width : s->width - o->x;
    const uint32_t ch = o->height ? o->height : s->height - o->y;
    if (cw > s->width - o->x || ch > s->height - o->y) return ESP_ERR_INVALID_SIZE;
    uint32_t rw = width, rh = height;
    if (o->resize == SOLAR_OS_TENSOR_LETTERBOX) {
        /* Floor scaled size, at least one pixel; split odd padding to right/bottom. */
        if ((uint64_t)width * ch <= (uint64_t)height * cw) {
            rh = (uint64_t)ch * width / cw; if (!rh) rh = 1;
        } else { rw = (uint64_t)cw * height / ch; if (!rw) rw = 1; }
    }
    *t = (solar_os_tensor_image_transform_t){
        .source_width = s->width, .source_height = s->height,
        .input_width = width, .input_height = height, .channels = channels,
        .crop_x = o->x, .crop_y = o->y, .crop_width = cw, .crop_height = ch,
        .resized_width = rw, .resized_height = rh,
        .pad_left = (width - rw) / 2, .pad_top = (height - rh) / 2, .bytes = bytes};
    return ESP_OK;
}
esp_err_t solar_os_tensor_image_prepare(const solar_os_raster_image_pixels_t *s,
    const solar_os_inference_tensor_t *p, const solar_os_tensor_image_options_t *o,
    uint8_t *dst, size_t length, solar_os_tensor_image_transform_t *t,
    solar_os_inference_cancel_fn cancel, void *user)
{
    const int64_t start = esp_timer_get_time();
    esp_err_t error = solar_os_tensor_image_inspect(s, p, o, t);
    if (error != ESP_OK) return error;
    if (!dst || length != t->bytes) return ESP_ERR_INVALID_SIZE;
    const uintptr_t a = (uintptr_t)s->data, b = (uintptr_t)dst;
    if (s->length > UINTPTR_MAX - a || length > UINTPTR_MAX - b ||
        (a < b + length && b < a + s->length)) return ESP_ERR_INVALID_ARG;
    if (cancel && cancel(user)) return ESP_ERR_TIMEOUT;
    uint32_t *lut = solar_os_memory_alloc(t->channels * 256U * sizeof(*lut),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "inference.image_lut");
    if (!lut) return ESP_ERR_NO_MEM;
    for (unsigned c = 0; c < t->channels; ++c) for (unsigned v = 0; v < 256; ++v) {
        double normalized = (v - o->mean[c]) / o->std[c];
        if (!isfinite(normalized)) { error = ESP_ERR_INVALID_ARG; goto done; }
        if (p->dtype == SOLAR_OS_TENSOR_FLOAT32) {
            if (fabs(normalized) > FLT_MAX) { error = ESP_ERR_INVALID_ARG; goto done; }
            float value = normalized; memcpy(&lut[c * 256 + v], &value, sizeof(value));
        } else {
            double value = ldexp(normalized, -p->exponents[p->exponent_count == 1 ? 0 : c]);
            const int lo = p->dtype == SOLAR_OS_TENSOR_INT8 ? -128 :
                p->dtype == SOLAR_OS_TENSOR_INT16 ? -32768 : 0;
            const int hi = p->dtype == SOLAR_OS_TENSOR_INT8 ? 127 :
                p->dtype == SOLAR_OS_TENSOR_INT16 ? 32767 : 255;
            int quantized = value <= lo ? lo : value >= hi ? hi : (int)floor(value + 0.5);
            lut[c * 256 + v] = (uint32_t)quantized;
        }
    }
    const bool planar = o->layout == SOLAR_OS_TENSOR_NCHW || o->layout == SOLAR_OS_TENSOR_CHW;
    const size_t element = p->dtype == SOLAR_OS_TENSOR_INT16 ? 2 :
        p->dtype == SOLAR_OS_TENSOR_FLOAT32 ? 4 : 1;
    const uint32_t step = t->crop_width / t->resized_width;
    const uint32_t remainder_step = t->crop_width % t->resized_width;
    for (uint32_t y = 0; y < t->input_height; ++y) {
        if (cancel && cancel(user)) { error = ESP_ERR_TIMEOUT; goto done; }
        const bool content_row = y >= t->pad_top && y - t->pad_top < t->resized_height;
        const uint32_t sy = content_row ? t->crop_y +
            (uint64_t)(y - t->pad_top) * t->crop_height / t->resized_height : 0;
        const uint8_t *row = content_row ? s->data + sy * s->stride : NULL;
        uint32_t sx = t->crop_x, remainder = 0;
        for (uint32_t x = 0; x < t->input_width; ++x) {
            uint8_t values[3];
            if (!content_row || x < t->pad_left || x - t->pad_left >= t->resized_width)
                memcpy(values, o->pad, sizeof(values));
            else {
                const uint8_t *rgb = row + sx * 3;
                if (o->color == SOLAR_OS_TENSOR_GRAY) {
                    values[0] = (77U * rgb[0] + 150U * rgb[1] + 29U * rgb[2]) >> 8;
                } else {
                    values[0] = rgb[o->color == SOLAR_OS_TENSOR_BGR ? 2 : 0]; values[1] = rgb[1];
                    values[2] = rgb[o->color == SOLAR_OS_TENSOR_BGR ? 0 : 2];
                }
                /* Exact floor-nearest stepping, avoiding division per pixel. */
                sx += step; remainder += remainder_step;
                if (remainder >= t->resized_width) { ++sx; remainder -= t->resized_width; }
            }
            for (unsigned c = 0; c < t->channels; ++c) {
                const uint32_t value = lut[c * 256 + values[c]];
                const size_t pixel = (size_t)y * t->input_width + x;
                const size_t index = planar ? c * (size_t)t->input_width * t->input_height + pixel : pixel * t->channels + c;
                for (size_t byte = 0; byte < element; ++byte) dst[index * element + byte] = value >> (8 * byte);
            }
        }
        if ((y & 15U) == 15U) vTaskDelay(1);
    }
    if (cancel && cancel(user)) error = ESP_ERR_TIMEOUT;
done:
    solar_os_memory_free(lut);
    t->preprocess_us = esp_timer_get_time() - start;
    return error;
}
