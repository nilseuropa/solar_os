#include "solar_os_view.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "solar_os_ble_keyboard.h"
#include "solar_os_gfx.h"
#include "solar_os_storage.h"
#include "solar_os_stb_image.h"
#include "solar_os_terminal.h"
#include "solar_os_vector.h"
#include "solar_os_webp_decoder.h"

#define VIEW_MAX_PIXELS (2U * 1024U * 1024U)
#define VIEW_GIF_MAX_CANVAS_PIXELS_PSRAM (512U * 512U)
#define VIEW_GIF_MAX_CANVAS_PIXELS_INTERNAL (160U * 120U)
#define VIEW_GIF_MAX_STORED_PIXELS_PSRAM VIEW_MAX_PIXELS
#define VIEW_GIF_MAX_STORED_PIXELS_INTERNAL (128U * 1024U)
#define VIEW_PAN_STEP 32
#define VIEW_TOKEN_MAX 32

static const char *TAG = "solar_os_view";
static char view_error_detail[96];

typedef enum {
    VIEW_MODE_FIT,
    VIEW_MODE_ACTUAL,
} view_mode_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *gray;
    uint32_t frame_count;
    uint32_t frame_index;
    uint32_t next_frame_ms;
    uint32_t *frame_delays_ms;
} view_image_t;

typedef struct {
    view_image_t image;
    view_mode_t mode;
    bool loaded;
    bool suspended;
    int pan_x;
    int pan_y;
    char path[SOLAR_OS_STORAGE_PATH_MAX];
} view_state_t;

static view_state_t view_state;

static uint16_t view_read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t view_read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
        ((uint32_t)data[1] << 8) |
        ((uint32_t)data[2] << 16) |
        ((uint32_t)data[3] << 24);
}

static int32_t view_read_le32s(const uint8_t *data)
{
    return (int32_t)view_read_le32(data);
}

static uint8_t *view_alloc(size_t len)
{
    uint8_t *data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (data == NULL) {
        data = heap_caps_malloc(len, MALLOC_CAP_8BIT);
    }
    return data;
}

static bool view_has_psram(void)
{
    return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
}

static uint32_t view_gif_max_canvas_pixels(void)
{
    return view_has_psram() ?
        VIEW_GIF_MAX_CANVAS_PIXELS_PSRAM :
        VIEW_GIF_MAX_CANVAS_PIXELS_INTERNAL;
}

static uint32_t view_gif_max_stored_pixels(void)
{
    return view_has_psram() ?
        VIEW_GIF_MAX_STORED_PIXELS_PSRAM :
        VIEW_GIF_MAX_STORED_PIXELS_INTERNAL;
}

static void view_free_image(view_image_t *image)
{
    if (image == NULL) {
        return;
    }

    heap_caps_free(image->gray);
    heap_caps_free(image->frame_delays_ms);
    image->gray = NULL;
    image->frame_delays_ms = NULL;
    image->width = 0;
    image->height = 0;
    image->frame_count = 0;
    image->frame_index = 0;
    image->next_frame_ms = 0;
}

static esp_err_t view_alloc_image(view_image_t *image, uint32_t width, uint32_t height)
{
    if (image == NULL || width == 0 || height == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint64_t pixels = (uint64_t)width * (uint64_t)height;
    if (pixels > VIEW_MAX_PIXELS || pixels > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *gray = view_alloc((size_t)pixels);
    if (gray == NULL) {
        return ESP_ERR_NO_MEM;
    }

    view_free_image(image);
    image->width = width;
    image->height = height;
    image->gray = gray;
    image->frame_count = 1;
    return ESP_OK;
}

static uint8_t *view_current_frame_gray(view_image_t *image)
{
    if (image == NULL || image->gray == NULL || image->width == 0 || image->height == 0) {
        return NULL;
    }

    const uint32_t frame_count = image->frame_count != 0 ? image->frame_count : 1U;
    if (image->frame_index >= frame_count) {
        image->frame_index = 0;
    }
    const size_t frame_pixels = (size_t)image->width * image->height;
    return image->gray + ((size_t)image->frame_index * frame_pixels);
}

static uint32_t view_current_frame_delay_ms(const view_image_t *image)
{
    if (image == NULL ||
        image->frame_count <= 1 ||
        image->frame_delays_ms == NULL ||
        image->frame_index >= image->frame_count) {
        return 100U;
    }

    const uint32_t delay = image->frame_delays_ms[image->frame_index];
    return delay != 0 ? delay : 100U;
}

static uint8_t view_rgb_to_gray(uint8_t red, uint8_t green, uint8_t blue)
{
    return (uint8_t)(((uint32_t)red * 77U +
                      (uint32_t)green * 150U +
                      (uint32_t)blue * 29U) >> 8);
}

static solar_os_gfx_color_t view_gray_to_color(uint8_t gray)
{
    const uint8_t level = (uint8_t)(((uint16_t)gray * SOLAR_OS_GFX_GRAY_MAX + 127U) / 255U);
    return solar_os_gfx_gray(level);
}

static bool view_read_exact(FILE *file, void *data, size_t len)
{
    return file != NULL && data != NULL && fread(data, 1, len, file) == len;
}

static esp_err_t view_read_stream(FILE *file, uint8_t **out_data, size_t *out_len)
{
    if (file == NULL || out_data == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = NULL;
    *out_len = 0;

    if (fseek(file, 0, SEEK_END) != 0) {
        return ESP_FAIL;
    }
    const long size = ftell(file);
    if (size <= 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((unsigned long)size > SIZE_MAX || (unsigned long)size > UINT32_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    uint8_t *data = view_alloc((size_t)size);
    if (data == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (!view_read_exact(file, data, (size_t)size)) {
        heap_caps_free(data);
        return ESP_FAIL;
    }

    *out_data = data;
    *out_len = (size_t)size;
    return ESP_OK;
}

static esp_err_t view_decode_stb(FILE *file, view_image_t *image, const char *format)
{
    uint8_t *image_data = NULL;
    size_t image_len = 0;

    esp_err_t err = view_read_stream(file, &image_data, &image_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *gray = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    err = solar_os_stb_decode_gray(image_data,
                                   image_len,
                                   VIEW_MAX_PIXELS,
                                   &gray,
                                   &width,
                                   &height);
    if (err == ESP_OK) {
        view_error_detail[0] = '\0';
        view_free_image(image);
        image->width = width;
        image->height = height;
        image->gray = gray;
        image->frame_count = 1;
        SOLAR_OS_LOGI(TAG, "decoded %s %" PRIu32 "x%" PRIu32 " bytes=%u",
                 format != NULL ? format : "image",
                 width,
                 height,
                 (unsigned)image_len);
    } else {
        snprintf(view_error_detail,
                 sizeof(view_error_detail),
                 "%s: %s",
                 format != NULL ? format : "image",
                 solar_os_stb_failure_reason());
        SOLAR_OS_LOGW(TAG,
                 "%s decode failed: %s reason=%s bytes=%u",
                 format != NULL ? format : "image",
                 esp_err_to_name(err),
                 solar_os_stb_failure_reason(),
                 (unsigned)image_len);
    }

    heap_caps_free(image_data);
    return err;
}

static esp_err_t view_decode_gif(FILE *file,
                                 view_image_t *image,
                                 uint32_t target_width,
                                 uint32_t target_height)
{
    uint8_t *image_data = NULL;
    size_t image_len = 0;

    esp_err_t err = view_read_stream(file, &image_data, &image_len);
    if (err != ESP_OK) {
        return err;
    }

    solar_os_stb_gif_animation_t animation = {0};
    err = solar_os_stb_decode_gif_gray(image_data,
                                       image_len,
                                       view_gif_max_canvas_pixels(),
                                       view_gif_max_stored_pixels(),
                                       target_width,
                                       target_height,
                                       solar_os_vector_rgba_to_gray_scaled,
                                       &animation);
    if (err == ESP_OK) {
        view_error_detail[0] = '\0';
        view_free_image(image);
        image->width = animation.width;
        image->height = animation.height;
        image->gray = animation.gray;
        image->frame_count = animation.frame_count;
        image->frame_delays_ms = animation.delays_ms;
        animation.gray = NULL;
        animation.delays_ms = NULL;
        SOLAR_OS_LOGI(TAG,
                      "decoded GIF %" PRIu32 "x%" PRIu32 " frames=%" PRIu32 " bytes=%u",
                      image->width,
                      image->height,
                      image->frame_count,
                      (unsigned)image_len);
    } else {
        const char *detail = solar_os_stb_failure_reason();
        if (err == ESP_ERR_INVALID_SIZE) {
            detail = "animation too large";
        } else if (err == ESP_ERR_NO_MEM) {
            detail = "not enough memory";
        }
        snprintf(view_error_detail,
                 sizeof(view_error_detail),
                 "GIF: %s",
                 detail);
        SOLAR_OS_LOGW(TAG,
                      "GIF decode failed: %s reason=%s bytes=%u",
                      esp_err_to_name(err),
                      detail,
                      (unsigned)image_len);
    }

    solar_os_stb_gif_animation_free(&animation);
    heap_caps_free(image_data);
    return err;
}

static esp_err_t view_decode_webp(FILE *file, view_image_t *image)
{
    uint8_t *image_data = NULL;
    size_t image_len = 0;

    esp_err_t err = view_read_stream(file, &image_data, &image_len);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *gray = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    err = solar_os_webp_decode_gray(image_data,
                                    image_len,
                                    VIEW_MAX_PIXELS,
                                    &gray,
                                    &width,
                                    &height);
    if (err == ESP_OK) {
        view_error_detail[0] = '\0';
        view_free_image(image);
        image->width = width;
        image->height = height;
        image->gray = gray;
        image->frame_count = 1;
        SOLAR_OS_LOGI(TAG, "decoded WebP %" PRIu32 "x%" PRIu32 " bytes=%u",
                 width,
                 height,
                 (unsigned)image_len);
    } else {
        snprintf(view_error_detail, sizeof(view_error_detail), "WebP decode failed");
        SOLAR_OS_LOGW(TAG,
                 "WebP decode failed: %s bytes=%u",
                 esp_err_to_name(err),
                 (unsigned)image_len);
    }

    heap_caps_free(image_data);
    return err;
}

static esp_err_t view_decode_bmp(FILE *file, view_image_t *image)
{
    uint8_t file_header[14];
    uint8_t dib_header[40];

    if (fseek(file, 0, SEEK_SET) != 0 ||
        !view_read_exact(file, file_header, sizeof(file_header)) ||
        !view_read_exact(file, dib_header, sizeof(dib_header))) {
        return ESP_FAIL;
    }

    if (file_header[0] != 'B' || file_header[1] != 'M') {
        return ESP_ERR_INVALID_ARG;
    }

    const uint32_t pixel_offset = view_read_le32(&file_header[10]);
    const uint32_t dib_size = view_read_le32(&dib_header[0]);
    const int32_t width_s = view_read_le32s(&dib_header[4]);
    const int32_t height_s = view_read_le32s(&dib_header[8]);
    const uint16_t planes = view_read_le16(&dib_header[12]);
    const uint16_t bits_per_pixel = view_read_le16(&dib_header[14]);
    const uint32_t compression = view_read_le32(&dib_header[16]);
    const uint32_t colors_used = view_read_le32(&dib_header[32]);

    if (dib_size < 40 || width_s <= 0 || height_s == 0 || planes != 1 || compression != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (bits_per_pixel != 1 &&
        bits_per_pixel != 4 &&
        bits_per_pixel != 8 &&
        bits_per_pixel != 24 &&
        bits_per_pixel != 32) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const uint32_t width = (uint32_t)width_s;
    const bool top_down = height_s < 0;
    const uint32_t height = top_down ? (uint32_t)(-height_s) : (uint32_t)height_s;
    const uint64_t bits_per_row = (uint64_t)width * bits_per_pixel;
    const uint64_t row_stride64 = ((bits_per_row + 31U) / 32U) * 4U;
    if (row_stride64 == 0 || row_stride64 > SIZE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const size_t row_stride = (size_t)row_stride64;

    uint8_t palette[256] = {0};
    if (bits_per_pixel <= 8) {
        uint32_t palette_entries = colors_used != 0 ? colors_used : (1UL << bits_per_pixel);
        if (palette_entries > 256) {
            return ESP_ERR_NOT_SUPPORTED;
        }
        if (fseek(file, 14L + (long)dib_size, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
        for (uint32_t i = 0; i < palette_entries; i++) {
            uint8_t entry[4];
            if (!view_read_exact(file, entry, sizeof(entry))) {
                return ESP_FAIL;
            }
            palette[i] = view_rgb_to_gray(entry[2], entry[1], entry[0]);
        }
    }

    esp_err_t err = view_alloc_image(image, width, height);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *row = view_alloc(row_stride);
    if (row == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t y = 0; y < height; y++) {
        const uint32_t file_y = top_down ? y : (height - 1U - y);
        const uint64_t row_offset = (uint64_t)pixel_offset + ((uint64_t)file_y * row_stride);
        if (row_offset > LONG_MAX || fseek(file, (long)row_offset, SEEK_SET) != 0) {
            heap_caps_free(row);
            return ESP_FAIL;
        }
        if (!view_read_exact(file, row, row_stride)) {
            heap_caps_free(row);
            return ESP_FAIL;
        }

        uint8_t *dest = &image->gray[(size_t)y * width];
        for (uint32_t x = 0; x < width; x++) {
            switch (bits_per_pixel) {
            case 1: {
                const uint8_t byte = row[x / 8U];
                const uint8_t index = (byte >> (7U - (x & 7U))) & 0x01U;
                dest[x] = palette[index];
                break;
            }
            case 4: {
                const uint8_t byte = row[x / 2U];
                const uint8_t index = (x & 1U) == 0 ? (byte >> 4) : (byte & 0x0fU);
                dest[x] = palette[index];
                break;
            }
            case 8:
                dest[x] = palette[row[x]];
                break;
            case 24: {
                const uint8_t *pixel = &row[(size_t)x * 3U];
                dest[x] = view_rgb_to_gray(pixel[2], pixel[1], pixel[0]);
                break;
            }
            case 32: {
                const uint8_t *pixel = &row[(size_t)x * 4U];
                dest[x] = view_rgb_to_gray(pixel[2], pixel[1], pixel[0]);
                break;
            }
            default:
                break;
            }
        }
    }

    heap_caps_free(row);
    return ESP_OK;
}

static int view_pnm_getc(FILE *file)
{
    return fgetc(file);
}

static bool view_pnm_next_token(FILE *file, char *token, size_t token_len)
{
    int ch;
    do {
        ch = view_pnm_getc(file);
        if (ch == '#') {
            do {
                ch = view_pnm_getc(file);
            } while (ch != EOF && ch != '\n' && ch != '\r');
        }
    } while (ch != EOF && isspace((unsigned char)ch));

    if (ch == EOF) {
        return false;
    }

    size_t len = 0;
    while (ch != EOF && !isspace((unsigned char)ch)) {
        if (ch == '#') {
            do {
                ch = view_pnm_getc(file);
            } while (ch != EOF && ch != '\n' && ch != '\r');
            break;
        }
        if (len + 1U < token_len) {
            token[len++] = (char)ch;
        }
        ch = view_pnm_getc(file);
    }
    token[len] = '\0';
    return len > 0;
}

static bool view_pnm_parse_u32(FILE *file, uint32_t *value)
{
    char token[VIEW_TOKEN_MAX];
    if (!view_pnm_next_token(file, token, sizeof(token))) {
        return false;
    }

    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(token, &end, 10);
    if (errno != 0 || end == token || *end != '\0' || parsed > UINT32_MAX) {
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static uint8_t view_scale_sample(uint32_t sample, uint32_t max_value)
{
    if (max_value == 0) {
        return 0;
    }
    if (sample > max_value) {
        sample = max_value;
    }
    return (uint8_t)((sample * 255U) / max_value);
}

static esp_err_t view_pnm_read_binary_sample(FILE *file,
                                             uint32_t max_value,
                                             uint8_t *out_sample)
{
    if (max_value < 256U) {
        const int value = fgetc(file);
        if (value == EOF) {
            return ESP_FAIL;
        }
        *out_sample = view_scale_sample((uint32_t)value, max_value);
        return ESP_OK;
    }

    const int high = fgetc(file);
    const int low = fgetc(file);
    if (high == EOF || low == EOF) {
        return ESP_FAIL;
    }
    *out_sample = view_scale_sample(((uint32_t)high << 8) | (uint32_t)low, max_value);
    return ESP_OK;
}

static esp_err_t view_decode_pnm(FILE *file, view_image_t *image)
{
    if (fseek(file, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    const int magic_p = fgetc(file);
    const int magic_type = fgetc(file);
    if (magic_p != 'P' || magic_type < '1' || magic_type > '6') {
        return ESP_ERR_INVALID_ARG;
    }

    const int type = magic_type - '0';
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t max_value = 1;
    if (!view_pnm_parse_u32(file, &width) || !view_pnm_parse_u32(file, &height)) {
        return ESP_FAIL;
    }
    if (type != 1 && type != 4) {
        if (!view_pnm_parse_u32(file, &max_value) || max_value == 0 || max_value > 65535U) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = view_alloc_image(image, width, height);
    if (err != ESP_OK) {
        return err;
    }

    if (type == 1 || type == 2 || type == 3) {
        for (uint32_t y = 0; y < height; y++) {
            uint8_t *dest = &image->gray[(size_t)y * width];
            for (uint32_t x = 0; x < width; x++) {
                uint32_t value = 0;
                if (!view_pnm_parse_u32(file, &value)) {
                    return ESP_FAIL;
                }
                if (type == 1) {
                    dest[x] = value == 0 ? 255 : 0;
                } else if (type == 2) {
                    dest[x] = view_scale_sample(value, max_value);
                } else {
                    uint32_t green = 0;
                    uint32_t blue = 0;
                    if (!view_pnm_parse_u32(file, &green) ||
                        !view_pnm_parse_u32(file, &blue)) {
                        return ESP_FAIL;
                    }
                    dest[x] = view_rgb_to_gray(view_scale_sample(value, max_value),
                                               view_scale_sample(green, max_value),
                                               view_scale_sample(blue, max_value));
                }
            }
        }
        return ESP_OK;
    }

    if (type == 4) {
        const size_t row_bytes = (width + 7U) / 8U;
        uint8_t *row = view_alloc(row_bytes);
        if (row == NULL) {
            return ESP_ERR_NO_MEM;
        }
        for (uint32_t y = 0; y < height; y++) {
            if (!view_read_exact(file, row, row_bytes)) {
                heap_caps_free(row);
                return ESP_FAIL;
            }
            uint8_t *dest = &image->gray[(size_t)y * width];
            for (uint32_t x = 0; x < width; x++) {
                const uint8_t byte = row[x / 8U];
                const bool black = ((byte >> (7U - (x & 7U))) & 0x01U) != 0;
                dest[x] = black ? 0 : 255;
            }
        }
        heap_caps_free(row);
        return ESP_OK;
    }

    for (uint32_t y = 0; y < height; y++) {
        uint8_t *dest = &image->gray[(size_t)y * width];
        for (uint32_t x = 0; x < width; x++) {
            if (type == 5) {
                err = view_pnm_read_binary_sample(file, max_value, &dest[x]);
                if (err != ESP_OK) {
                    return err;
                }
            } else {
                uint8_t red;
                uint8_t green;
                uint8_t blue;
                err = view_pnm_read_binary_sample(file, max_value, &red);
                if (err == ESP_OK) {
                    err = view_pnm_read_binary_sample(file, max_value, &green);
                }
                if (err == ESP_OK) {
                    err = view_pnm_read_binary_sample(file, max_value, &blue);
                }
                if (err != ESP_OK) {
                    return err;
                }
                dest[x] = view_rgb_to_gray(red, green, blue);
            }
        }
    }

    return ESP_OK;
}

static esp_err_t view_decode_file(const char *path,
                                  view_image_t *image,
                                  uint32_t target_width,
                                  uint32_t target_height)
{
    if (path == NULL || image == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }

    view_error_detail[0] = '\0';
    uint8_t signature[12] = {0};
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    const size_t signature_len = fread(signature, 1, sizeof(signature), file);
    if (signature_len >= 2U) {
        if (signature[0] == 'B' && signature[1] == 'M') {
            err = view_decode_bmp(file, image);
        } else if (signature[0] == 0xff && signature[1] == 0xd8) {
            err = view_decode_stb(file, image, "JPEG");
        } else if (signature_len >= 8U &&
                   signature[0] == 0x89 &&
                   signature[1] == 'P' &&
                   signature[2] == 'N' &&
                   signature[3] == 'G' &&
                   signature[4] == 0x0d &&
                   signature[5] == 0x0a &&
                   signature[6] == 0x1a &&
                   signature[7] == 0x0a) {
            err = view_decode_stb(file, image, "PNG");
        } else if (signature[0] == 'G' && signature[1] == 'I') {
            err = view_decode_gif(file, image, target_width, target_height);
        } else if (signature_len >= 12U &&
                   memcmp(signature, "RIFF", 4) == 0 &&
                   memcmp(signature + 8, "WEBP", 4) == 0) {
            err = view_decode_webp(file, image);
        } else if (signature[0] == 'P' && signature[1] >= '1' && signature[1] <= '6') {
            err = view_decode_pnm(file, image);
        }
    } else {
        err = ESP_FAIL;
    }

    fclose(file);
    if (err != ESP_OK) {
        view_free_image(image);
    }
    return err;
}

static void view_usage(solar_os_terminal_t *term)
{
    solar_os_terminal_writeln(term, "usage: view [-fit|-actual] <image>");
    solar_os_terminal_writeln(term, "formats: JPG, JPEG, PNG, GIF/animated GIF, WEBP, BMP, PBM, PGM, PPM");
    solar_os_terminal_writeln(term, "keys: arrows pan, f toggles fit/actual");
    solar_os_terminal_writeln(term, "CTRL+ALT+DEL exits");
}

static void view_print_error(solar_os_terminal_t *term, const char *path, esp_err_t err)
{
    solar_os_terminal_clear(term);
    solar_os_terminal_writeln_bold(term, "view");
    if (path != NULL && path[0] != '\0') {
        solar_os_terminal_printf(term, "file: %s\n", path);
    }
    solar_os_terminal_printf(term, "error: %s\n", esp_err_to_name(err));
    if (view_error_detail[0] != '\0') {
        solar_os_terminal_printf(term, "detail: %s\n", view_error_detail);
    }
    solar_os_terminal_writeln(term, "");
    view_usage(term);
}

static void view_reset_pan(solar_os_gfx_t *gfx)
{
    if (gfx == NULL || view_state.image.gray == NULL) {
        view_state.pan_x = 0;
        view_state.pan_y = 0;
        return;
    }

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    const int image_width = (int)view_state.image.width;
    const int image_height = (int)view_state.image.height;
    view_state.pan_x = image_width > screen_width ? (screen_width - image_width) / 2 : 0;
    view_state.pan_y = image_height > screen_height ? (screen_height - image_height) / 2 : 0;
}

static void view_clamp_pan(solar_os_gfx_t *gfx)
{
    if (gfx == NULL) {
        return;
    }

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    const int image_width = (int)view_state.image.width;
    const int image_height = (int)view_state.image.height;

    if (image_width > screen_width) {
        const int min_x = screen_width - image_width;
        if (view_state.pan_x < min_x) {
            view_state.pan_x = min_x;
        }
        if (view_state.pan_x > 0) {
            view_state.pan_x = 0;
        }
    } else {
        view_state.pan_x = 0;
    }

    if (image_height > screen_height) {
        const int min_y = screen_height - image_height;
        if (view_state.pan_y < min_y) {
            view_state.pan_y = min_y;
        }
        if (view_state.pan_y > 0) {
            view_state.pan_y = 0;
        }
    } else {
        view_state.pan_y = 0;
    }
}

static void view_fit_dimensions(int image_width,
                                int image_height,
                                int screen_width,
                                int screen_height,
                                int *draw_width,
                                int *draw_height)
{
    if (draw_width == NULL || draw_height == NULL ||
        image_width <= 0 || image_height <= 0 ||
        screen_width <= 0 || screen_height <= 0) {
        return;
    }

    const uint64_t height_for_width =
        ((uint64_t)screen_width * (uint64_t)image_height) / (uint64_t)image_width;
    if (height_for_width <= (uint64_t)screen_height) {
        *draw_width = screen_width;
        *draw_height = height_for_width > 0 ? (int)height_for_width : 1;
        return;
    }

    const uint64_t width_for_height =
        ((uint64_t)screen_height * (uint64_t)image_width) / (uint64_t)image_height;
    *draw_width = width_for_height > 0 ? (int)width_for_height : 1;
    *draw_height = screen_height;
}

static void view_draw_scaled(solar_os_gfx_t *gfx,
                             int origin_x,
                             int origin_y,
                             int draw_width,
                             int draw_height)
{
    const view_image_t *image = &view_state.image;
    uint8_t *gray = view_current_frame_gray(&view_state.image);
    if (gray == NULL) {
        return;
    }

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    const int clip_x0 = origin_x < 0 ? 0 : origin_x;
    const int clip_y0 = origin_y < 0 ? 0 : origin_y;
    int clip_x1 = origin_x + draw_width;
    int clip_y1 = origin_y + draw_height;
    if (clip_x1 > screen_width) {
        clip_x1 = screen_width;
    }
    if (clip_y1 > screen_height) {
        clip_y1 = screen_height;
    }
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return;
    }

    for (int dy = clip_y0; dy < clip_y1; dy++) {
        const uint32_t sy =
            (uint32_t)(((uint64_t)(dy - origin_y) * image->height) / (uint32_t)draw_height);
        solar_os_gfx_color_t run_color = SOLAR_OS_GFX_COLOR_WHITE;
        int run_start = clip_x0;
        bool run_active = false;

        for (int dx = clip_x0; dx < clip_x1; dx++) {
            const uint32_t sx =
                (uint32_t)(((uint64_t)(dx - origin_x) * image->width) / (uint32_t)draw_width);
            const uint8_t sample = gray[(size_t)sy * image->width + sx];
            const solar_os_gfx_color_t color = view_gray_to_color(sample);
            if (!run_active) {
                run_active = true;
                run_color = color;
                run_start = dx;
            } else if (color != run_color) {
                solar_os_gfx_set_color(gfx, run_color);
                solar_os_gfx_fill_rect(gfx, run_start, dy, dx - run_start, 1);
                run_color = color;
                run_start = dx;
            }
        }

        if (run_active) {
            solar_os_gfx_set_color(gfx, run_color);
            solar_os_gfx_fill_rect(gfx, run_start, dy, clip_x1 - run_start, 1);
        }
    }
}

static void view_render(solar_os_context_t *ctx)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    if (gfx == NULL || view_state.suspended || view_state.image.gray == NULL) {
        return;
    }

    const int screen_width = (int)solar_os_gfx_width(gfx);
    const int screen_height = (int)solar_os_gfx_height(gfx);
    int draw_width = (int)view_state.image.width;
    int draw_height = (int)view_state.image.height;
    int origin_x = 0;
    int origin_y = 0;

    if (view_state.mode == VIEW_MODE_FIT) {
        view_fit_dimensions((int)view_state.image.width,
                            (int)view_state.image.height,
                            screen_width,
                            screen_height,
                            &draw_width,
                            &draw_height);
    }

    if (view_state.mode == VIEW_MODE_ACTUAL) {
        view_clamp_pan(gfx);
        origin_x = draw_width > screen_width ? view_state.pan_x : (screen_width - draw_width) / 2;
        origin_y = draw_height > screen_height ? view_state.pan_y : (screen_height - draw_height) / 2;
    } else {
        origin_x = (screen_width - draw_width) / 2;
        origin_y = (screen_height - draw_height) / 2;
    }

    solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
    view_draw_scaled(gfx, origin_x, origin_y, draw_width, draw_height);
    solar_os_gfx_present(gfx);
}

static void view_advance_animation(solar_os_context_t *ctx, uint32_t now_ms)
{
    view_image_t *image = &view_state.image;
    if (!view_state.loaded ||
        view_state.suspended ||
        image->gray == NULL ||
        image->frame_count <= 1) {
        return;
    }

    if (image->next_frame_ms == 0) {
        image->next_frame_ms = now_ms + view_current_frame_delay_ms(image);
        return;
    }
    if ((int32_t)(now_ms - image->next_frame_ms) < 0) {
        return;
    }

    image->frame_index = (image->frame_index + 1U) % image->frame_count;
    image->next_frame_ms = now_ms + view_current_frame_delay_ms(image);
    view_render(ctx);
}

static bool view_parse_args(solar_os_context_t *ctx, view_mode_t *mode, const char **path_arg)
{
    if (ctx == NULL || mode == NULL || path_arg == NULL) {
        return false;
    }

    *mode = VIEW_MODE_FIT;
    *path_arg = NULL;

    const int argc = solar_os_context_argc(ctx);
    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (arg == NULL) {
            return false;
        }
        if (strcmp(arg, "-fit") == 0 || strcmp(arg, "--fit") == 0) {
            *mode = VIEW_MODE_FIT;
        } else if (strcmp(arg, "-actual") == 0 || strcmp(arg, "--actual") == 0) {
            *mode = VIEW_MODE_ACTUAL;
        } else if (*path_arg == NULL) {
            *path_arg = arg;
        } else {
            return false;
        }
    }

    return *path_arg != NULL;
}

static esp_err_t view_start(solar_os_context_t *ctx)
{
    memset(&view_state, 0, sizeof(view_state));
    view_error_detail[0] = '\0';

    solar_os_terminal_t *term = solar_os_context_terminal(ctx);
    view_mode_t mode;
    const char *path_arg = NULL;
    if (!view_parse_args(ctx, &mode, &path_arg)) {
        solar_os_terminal_clear(term);
        view_usage(term);
        return ESP_OK;
    }

    esp_err_t err = solar_os_storage_resolve_path(path_arg,
                                                  view_state.path,
                                                  sizeof(view_state.path));
    if (err != ESP_OK) {
        view_print_error(term, path_arg, err);
        return ESP_OK;
    }

    struct stat st;
    if (stat(view_state.path, &st) != 0 || !S_ISREG(st.st_mode)) {
        view_print_error(term, view_state.path, ESP_ERR_NOT_FOUND);
        return ESP_OK;
    }

    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    const uint32_t target_width = gfx != NULL ? (uint32_t)solar_os_gfx_width(gfx) : 0;
    const uint32_t target_height = gfx != NULL ? (uint32_t)solar_os_gfx_height(gfx) : 0;
    err = view_decode_file(view_state.path, &view_state.image, target_width, target_height);
    if (err != ESP_OK) {
        view_print_error(term, view_state.path, err);
        return ESP_OK;
    }

    view_state.loaded = true;
    view_state.suspended = false;
    view_state.mode = mode;
    solar_os_context_set_graphics_active(ctx, true);
    view_reset_pan(solar_os_context_gfx(ctx));
    view_render(ctx);
    return ESP_OK;
}

static void view_stop(solar_os_context_t *ctx)
{
    view_free_image(&view_state.image);
    memset(&view_state, 0, sizeof(view_state));
    solar_os_context_set_graphics_active(ctx, false);
}

static void view_suspend(solar_os_context_t *ctx)
{
    view_state.suspended = true;
    solar_os_context_set_graphics_active(ctx, false);
}

static void view_resume(solar_os_context_t *ctx)
{
    view_state.suspended = false;
    if (view_state.loaded) {
        solar_os_context_set_graphics_active(ctx, true);
        view_state.image.next_frame_ms = 0;
        view_render(ctx);
    }
}

static void view_title(solar_os_context_t *ctx, char *buffer, size_t buffer_len)
{
    (void)ctx;
    if (buffer == NULL || buffer_len == 0) {
        return;
    }
    const char *slash = strrchr(view_state.path, '/');
    const char *name = slash != NULL && slash[1] != '\0' ? slash + 1 : view_state.path;
    if (name != NULL && name[0] != '\0') {
        snprintf(buffer, buffer_len, "view %s", name);
        return;
    }
    strlcpy(buffer, "view", buffer_len);
}

static bool view_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_RESUME) {
        view_resume(ctx);
        return true;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        view_advance_animation(ctx, event->data.tick_ms);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE) {
        solar_os_context_request_exit(ctx);
        return true;
    }
    if (!view_state.loaded) {
        return true;
    }

    bool redraw = false;
    switch (ch) {
    case SOLAR_OS_KEY_LEFT:
        view_state.pan_x += VIEW_PAN_STEP;
        redraw = true;
        break;
    case SOLAR_OS_KEY_RIGHT:
        view_state.pan_x -= VIEW_PAN_STEP;
        redraw = true;
        break;
    case SOLAR_OS_KEY_UP:
        view_state.pan_y += VIEW_PAN_STEP;
        redraw = true;
        break;
    case SOLAR_OS_KEY_DOWN:
        view_state.pan_y -= VIEW_PAN_STEP;
        redraw = true;
        break;
    case 'f':
    case 'F':
        view_state.mode = view_state.mode == VIEW_MODE_FIT ? VIEW_MODE_ACTUAL : VIEW_MODE_FIT;
        view_reset_pan(solar_os_context_gfx(ctx));
        redraw = true;
        break;
    case '0':
        view_state.mode = VIEW_MODE_ACTUAL;
        view_reset_pan(solar_os_context_gfx(ctx));
        redraw = true;
        break;
    case '1':
        view_state.mode = VIEW_MODE_FIT;
        redraw = true;
        break;
    default:
        break;
    }

    if (redraw) {
        view_render(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_view_app = {
    .name = "view",
    .summary = "image viewer",
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = view_start,
    .suspend = view_suspend,
    .resume = view_resume,
    .stop = view_stop,
    .event = view_event,
    .title = view_title,
};
