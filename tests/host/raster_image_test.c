#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "solar_os_raster_image.h"
#include "solar_os_gfx.h"
#include "solar_os_memory.h"

static unsigned allocations, decoded, direct, draws, presents;
static bool fail_alloc, color = true;
struct solar_os_gfx { int unused; };
static const uint8_t rgb[] = {255, 0, 0, 0, 255, 0, 0, 0, 255};
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    (void)tag; assert(kind == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED || kind == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    if (fail_alloc) return NULL;
    void *p = malloc(size); if (p) ++allocations; return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{ void *p = solar_os_memory_alloc(n * size, kind, tag); if (p) memset(p, 0, n * size); return p; }
void solar_os_memory_free(void *p) { if (p) { assert(allocations); --allocations; free(p); } }
esp_err_t solar_os_stb_decode_rgb(const uint8_t *data, size_t length, uint32_t max,
    uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    assert(max == 2U * 1024U * 1024U);
    if (!length || data[0] != 'R') return ESP_ERR_INVALID_RESPONSE;
    *pixels = solar_os_memory_alloc(sizeof(rgb), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "test");
    if (!*pixels) return ESP_ERR_NO_MEM;
    memcpy(*pixels, rgb, sizeof(rgb)); *width = 3; *height = 1; ++decoded; return ESP_OK;
}
esp_err_t solar_os_webp_decode_rgb(const uint8_t *data, size_t length, uint32_t max,
    uint8_t **pixels, uint32_t *width, uint32_t *height)
{ return solar_os_stb_decode_rgb(data, length, max, pixels, width, height); }
void solar_os_stb_image_free(void *p) { solar_os_memory_free(p); }
void solar_os_webp_free(void *p) { solar_os_memory_free(p); }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx; return 320; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx; return 240; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx, solar_os_display_format_t format)
{ (void)gfx; assert(format == SOLAR_OS_DISPLAY_FORMAT_RGB565); return color; }
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame)
{
    (void)gfx; assert(frame->format == SOLAR_OS_DISPLAY_FORMAT_RGB565 && frame->source_width == 3);
    static const uint8_t expected[] = {0xf8, 0, 0x07, 0xe0, 0, 0x1f};
    assert(frame->data_size == sizeof(expected) && !memcmp(frame->data, expected, sizeof(expected)));
    ++direct; return ESP_OK;
}
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx, const solar_os_gfx_raster_t *raster,
    int x, int y, int width, int height, const solar_os_gfx_clip_t *options)
{
    (void)gfx; (void)x; (void)y; (void)width; (void)height; (void)options;
    assert(raster->format == SOLAR_OS_GFX_RASTER_RGB888 && !memcmp(raster->pixels, rgb, sizeof(rgb)));
    ++draws; return ESP_OK;
}
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; ++presents; }

int main(void)
{
    solar_os_raster_image_t *image = NULL;
    struct solar_os_gfx gfx;
    assert(solar_os_raster_image_decode(NULL, 0, &image) == ESP_ERR_INVALID_SIZE && !image);
    assert(solar_os_raster_image_decode((const uint8_t *)"R", 4U * 1024U * 1024U + 1U, &image) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_raster_image_decode((const uint8_t *)"bad", 3, &image) == ESP_ERR_INVALID_RESPONSE && !allocations);
    fail_alloc = true;
    assert(solar_os_raster_image_decode((const uint8_t *)"R", 1, &image) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    assert(solar_os_raster_image_decode((const uint8_t *)"R", 1, &image) == ESP_OK);
    assert(solar_os_raster_image_width(image) == 3 && solar_os_raster_image_height(image) == 1);
    solar_os_raster_image_pixels_t pixels;
    assert(solar_os_raster_image_pixels(image, &pixels) == ESP_OK);
    assert(pixels.width == 3 && pixels.stride == 9 && pixels.length == 9);
    assert(!memcmp(pixels.data, rgb, sizeof(rgb)));
    solar_os_raster_image_convert_options_t opt = {.format = SOLAR_OS_RASTER_IMAGE_GRAY8};
    assert(solar_os_raster_image_convert(image, &opt, (uint8_t *)pixels.data, pixels.length, 0) == ESP_ERR_INVALID_ARG);
    uint8_t converted[20]; memset(converted, 0xab, sizeof(converted));
    assert(solar_os_raster_image_convert(image, &opt, converted, 3, 0) == ESP_OK);
    assert(converted[0] == 76 && converted[1] == 149 && converted[2] == 28 && converted[3] == 0xab);
    opt.format = SOLAR_OS_RASTER_IMAGE_RGB565_LE;
    assert(solar_os_raster_image_convert(image, &opt, converted, 6, 0) == ESP_OK);
    const uint8_t le565[] = {0, 0xf8, 0xe0, 7, 0x1f, 0};
    assert(!memcmp(converted, le565, sizeof(le565)));
    opt = (solar_os_raster_image_convert_options_t){.x = 1, .width = 1,
        .output_width = 2, .output_height = 2, .format = SOLAR_OS_RASTER_IMAGE_RGB888};
    memset(converted, 0xab, sizeof(converted));
    assert(solar_os_raster_image_convert(image, &opt, converted, 14, 8) == ESP_OK);
    const uint8_t green[] = {0, 255, 0, 0, 255, 0};
    assert(!memcmp(converted, green, 6) && !memcmp(converted + 8, green, 6));
    assert(converted[6] == 0xab && converted[7] == 0xab && converted[14] == 0xab);
    assert(solar_os_raster_image_convert(image, &opt, converted, 13, 8) == ESP_ERR_INVALID_SIZE);
    assert(solar_os_raster_image_convert(image, &opt, converted, 20, SIZE_MAX) == ESP_ERR_INVALID_SIZE);
    opt.x = UINT32_MAX;
    assert(solar_os_raster_image_convert(image, &opt, converted, 20, 0) == ESP_ERR_INVALID_SIZE);
    opt.x = 0; opt.width = UINT32_MAX;
    assert(solar_os_raster_image_convert(image, &opt, converted, 20, 0) == ESP_ERR_INVALID_SIZE);
    opt.width = 1; opt.output_width = UINT32_MAX;
    assert(solar_os_raster_image_convert(image, &opt, converted, 20, 0) == ESP_ERR_INVALID_SIZE);
    opt.output_width = 1; opt.format = (solar_os_raster_image_format_t)99;
    assert(solar_os_raster_image_convert(image, &opt, converted, 20, 0) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_raster_image_pixels(NULL, &pixels) == ESP_ERR_INVALID_ARG && !pixels.data);
    assert(solar_os_raster_image_draw(image, &gfx, -1, 0, 30, 10) == ESP_OK);
    fail_alloc = true;
    assert(solar_os_raster_image_present(image, &gfx, 0, 0, 30, 10) == ESP_ERR_NO_MEM);
    fail_alloc = false;
    assert(solar_os_raster_image_present(image, &gfx, 0, 0, 30, 10) == ESP_OK);
    unsigned count = allocations;
    assert(solar_os_raster_image_present(image, &gfx, 0, 0, 30, 10) == ESP_OK && allocations == count);
    assert(solar_os_raster_image_present(image, &gfx, -1, 0, 30, 10) == ESP_ERR_INVALID_ARG);
    assert(solar_os_raster_image_present(image, &gfx, 310, 0, 30, 10) == ESP_ERR_INVALID_SIZE);
    color = false;
    assert(solar_os_raster_image_present(image, &gfx, 0, 0, 30, 10) == ESP_OK && presents == 1);
    solar_os_raster_image_retain(image); solar_os_raster_image_release(image);
    assert(allocations == count);
    solar_os_raster_image_release(image); assert(!allocations);
    const uint8_t webp[] = "RIFFxxxxWEBP";
    assert(solar_os_raster_image_decode(webp, 12, &image) == ESP_OK);
    solar_os_raster_image_release(image); assert(!allocations);
    char path[] = "/tmp/solar-raster-test.XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0 && write(fd, "R", 1) == 1); close(fd);
    assert(solar_os_raster_image_open(path, &image) == ESP_OK); unlink(path);
    solar_os_raster_image_release(image); assert(!allocations);
    assert(decoded == 3 && direct == 2 && draws == 2);
    puts("raster decode/native RGB565 presentation tests passed");
}
