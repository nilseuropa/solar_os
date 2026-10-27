#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "solar_os_frame_presenter.h"

struct solar_os_frame_presenter {
    solar_os_frame_presenter_config_t config;
    uint8_t *presenting, *mono;
    size_t source_size, mono_size;
    uint16_t output_x, output_y, output_width, output_height, output_stride;
    bool mono_fallback, clear_background_pending;
    uint8_t mono_luma[256];
};
static bool inverted;
static solar_os_display_raster_t captured;
bool solar_os_gfx_palette_inverted(const solar_os_gfx_t *gfx)
{ assert(gfx); return inverted; }
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame)
{ assert(gfx && frame); captured = *frame; return ESP_OK; }

#include "frame_palette.inc"

static void compare_conversion(void)
{
    static const unsigned dimensions[][4] = {
        {1,1,1,1}, {7,5,7,5}, {13,11,7,5}, {321,17,321,17},
        {321,17,321,31}, {17,31,17,3},
        {400,300,400,300}, {320,240,400,300}, {640,256,426,320},
        {160,144,328,300}, {1024,768,64,64},
    };
    static const unsigned rank[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    uint16_t palette[256];
    for (unsigned i = 0; i < 256; ++i) palette[i] = (i * 257U) ^ (i << 4);
    for (unsigned format = SOLAR_OS_DISPLAY_FORMAT_INDEX8;
         format <= SOLAR_OS_DISPLAY_FORMAT_INDEX2; ++format)
        for (unsigned d = 0; d < sizeof(dimensions) / sizeof(dimensions[0]); ++d) {
            unsigned sw = dimensions[d][0], sh = dimensions[d][1];
            unsigned ow = dimensions[d][2], oh = dimensions[d][3];
            unsigned stride = (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? sw : (sw + 3) / 4) + 3;
            unsigned outstride = (ow + 7) / 8 + 2, bytes = outstride * oh;
            uint8_t *pixels = malloc(stride * sh), *guard = malloc(bytes + 16), *expected = calloc(1, bytes);
            assert(pixels && guard && expected);
            for (unsigned i = 0; i < stride * sh; ++i) pixels[i] = (i * 73U + 19U) ^ (i >> 5);
            memset(guard, 0xa5, bytes + 16);
            solar_os_frame_presenter_t p = {
                .config = {.format = format, .width = sw, .height = sh, .stride = stride,
                    .palette_rgb565 = palette, .palette_size = 256},
                .presenting = pixels, .mono = guard + 8, .mono_size = bytes,
                .output_width = ow, .output_height = oh, .output_stride = outstride, .mono_fallback = true,
            };
            frame_prepare_mono_palette(&p);
            for (unsigned y = 0; y < oh; ++y)
                for (unsigned x = 0; x < ow; ++x) {
                    unsigned sx = x * sw / ow, sy = y * sh / oh;
                    unsigned index = format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? pixels[sy * stride + sx] :
                        (pixels[sy * stride + sx / 4] >> ((sx % 4) * 2)) & 3;
                    uint16_t c = palette[index];
                    unsigned luma = format == SOLAR_OS_DISPLAY_FORMAT_INDEX2 ? 255U - index * 85U :
                        ((((c >> 11) & 31U) * 255U / 31U * 77U +
                          ((c >> 5) & 63U) * 255U / 63U * 150U +
                          (c & 31U) * 255U / 31U * 29U) >> 8);
                    if (rank[(y % 4) * 4 + x % 4] * 16 < 255U - luma)
                        expected[y * outstride + x / 8] |= 1U << (x % 8);
                }
            frame_convert_to_mono(&p);
            assert(!memcmp(expected, p.mono, bytes));
            for (unsigned i = 0; i < 8; ++i) assert(guard[i] == 0xa5 && guard[bytes + 8 + i] == 0xa5);
            free(pixels); free(guard); free(expected);
        }
}

static unsigned reference_luma(unsigned color)
{
    const unsigned r = ((color >> 11) & 31U) * 255U / 31U;
    const unsigned g = ((color >> 5) & 63U) * 255U / 63U;
    const unsigned b = (color & 31U) * 255U / 31U;
    return (77U * r + 150U * g + 29U * b) >> 8;
}

static void compare_rgb565_conversion(void)
{
    static const unsigned dimensions[][4] = {
        {1,1,1,1}, {7,5,7,5}, {13,11,7,5}, {321,17,321,31},
        {400,300,400,300}, {320,240,400,300}, {1024,768,64,64},
        /* Every RGB565 color at each of the sixteen dither thresholds. */
        {1024,256,1024,256},
    };
    static const unsigned rank[16] = {0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5};
    for (unsigned d = 0; d < sizeof(dimensions) / sizeof(dimensions[0]); ++d) {
        unsigned sw = dimensions[d][0], sh = dimensions[d][1];
        unsigned ow = dimensions[d][2], oh = dimensions[d][3];
        unsigned stride = sw * 2 + 3, outstride = (ow + 7) / 8 + 2, bytes = outstride * oh;
        uint8_t *allocation = malloc(stride * sh + 1);
        uint8_t *guard = malloc(bytes + 16), *expected = calloc(1, bytes);
        assert(allocation && guard && expected);
        uint8_t *pixels = allocation + 1;
        memset(pixels, 0xda, stride * sh);
        memset(guard, 0xa5, bytes + 16);
        solar_os_frame_presenter_t p = {
            .config = {.gfx = (solar_os_gfx_t *)&captured,
                .format = SOLAR_OS_DISPLAY_FORMAT_RGB565, .width = sw, .height = sh,
                .stride = stride, .literal_mono_palette = true},
            .presenting = pixels, .mono = guard + 8, .mono_size = bytes,
            .source_size = stride * sh, .mono_fallback = true,
            .output_width = ow, .output_height = oh, .output_stride = outstride,
        };
        /* No palette exists for RGB input, including during preparation. */
        frame_prepare_mono_palette(&p);
        for (unsigned phase = 0; phase < 4; ++phase) {
            /* Shift the color sequence across all horizontal Bayer phases. */
            for (unsigned y = 0; y < sh; ++y)
                for (unsigned x = 0; x < sw; ++x) {
                    unsigned color = ((y / 4) * 1024 + x + phase) & 0xffffU;
                    pixels[y * stride + x * 2] = color >> 8;
                    pixels[y * stride + x * 2 + 1] = color;
                }
            memset(expected, 0, bytes);
            for (unsigned y = 0; y < oh; ++y)
                for (unsigned x = 0; x < ow; ++x) {
                    unsigned offset = (y * sh / oh) * stride + (x * sw / ow) * 2;
                    unsigned color = ((unsigned)pixels[offset] << 8) | pixels[offset + 1];
                    if (rank[(y % 4) * 4 + x % 4] * 16 < 255U - reference_luma(color))
                        expected[y * outstride + x / 8] |= 1U << (x % 8);
                }
            inverted = phase & 1U;
            p.config.literal_mono_palette = (phase & 2U) != 0;
            assert(frame_present(&p) == ESP_OK);
            assert(captured.format == SOLAR_OS_DISPLAY_FORMAT_MONO1);
            assert(captured.data == p.mono && captured.data_size == bytes);
            assert(captured.source_width == ow && captured.source_height == oh);
            assert(captured.source_stride == outstride);
            assert(captured.palette_inverted == (inverted && !p.config.literal_mono_palette));
            assert(!captured.palette_rgb565 && captured.palette_size == 0);
            assert(!memcmp(expected, p.mono, bytes));
            for (unsigned i = 0; i < 8; ++i) assert(guard[i] == 0xa5 && guard[bytes + 8 + i] == 0xa5);
        }
        free(allocation); free(guard); free(expected);
    }
    /* Black and white are exact endpoints. A midtone retains a spatial pattern. */
    uint8_t pixels[8 * 4 * 2], mono[4];
    solar_os_frame_presenter_t p = {
        .config = {.format = SOLAR_OS_DISPLAY_FORMAT_RGB565, .width = 8, .height = 4, .stride = 16},
        .presenting = pixels, .mono = mono, .mono_size = sizeof(mono),
        .output_width = 8, .output_height = 4, .output_stride = 1,
    };
    memset(pixels, 0xff, sizeof(pixels)); frame_convert_to_mono(&p);
    for (unsigned i = 0; i < sizeof(mono); ++i) assert(mono[i] == 0);
    memset(pixels, 0, sizeof(pixels)); frame_convert_to_mono(&p);
    for (unsigned i = 0; i < sizeof(mono); ++i) assert(mono[i] == 0xff);
    for (unsigned i = 0; i < sizeof(pixels); i += 2) { pixels[i] = 0x84; pixels[i + 1] = 0x10; }
    frame_convert_to_mono(&p);
    assert(mono[0] != mono[1] && mono[0] != 0 && mono[0] != 0xff);
}

int main(void)
{
    compare_conversion();
    compare_rgb565_conversion();
    /* Direct RGB565 is already in wire byte order. Preserve its pointer,
     * bytes and background color for every terminal palette/polarity setting. */
    uint8_t rgb[] = {0xf8,0, 0x07,0xe0, 0,0x1f, 0xff,0xff};
    for (unsigned polarity = 0; polarity < 2; polarity++)
        for (unsigned literal = 0; literal < 2; literal++)
            for (unsigned reverse = 0; reverse < 2; reverse++) {
                inverted = polarity;
                solar_os_frame_presenter_t p = {
                    .config = {.gfx = (solar_os_gfx_t *)&captured,
                        .format = SOLAR_OS_DISPLAY_FORMAT_RGB565, .width = 2,
                        .height = 2, .stride = 4, .literal_direct_palette = literal,
                        .reverse_direct_palette = reverse, .background_rgb565 = 0x1234},
                    .presenting = rgb, .source_size = sizeof(rgb),
                    .output_width = 4, .output_height = 4, .clear_background_pending = true,
                };
                assert(frame_present(&p) == ESP_OK);
                assert(captured.data == rgb && captured.format == SOLAR_OS_DISPLAY_FORMAT_RGB565);
                assert(captured.data_size == sizeof(rgb) && captured.source_stride == 4);
                assert(!captured.palette_inverted && captured.palette_rgb565 == NULL);
                assert(captured.background_rgb565 == 0x1234 && captured.clear_background);
                assert(rgb[0] == 0xf8 && rgb[1] == 0);
            }
    uint16_t palette[256] = {[0] = 0x00ffU, [3] = 0xf800U, [32] = 0, [33] = 0xffffU};
    uint8_t pixels[32], mono[4];
    for (unsigned format = SOLAR_OS_DISPLAY_FORMAT_INDEX8;
         format <= SOLAR_OS_DISPLAY_FORMAT_INDEX2; ++format) {
        solar_os_frame_presenter_t p = {
            .config = {.gfx = (solar_os_gfx_t *)&captured, .format = format,
                .width = 8, .height = 4, .stride = format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 8 : 2,
                .palette_rgb565 = palette, .palette_size = 256, .background_index = 33},
            .presenting = pixels, .mono = mono, .source_size = sizeof(pixels), .mono_size = sizeof(mono),
            .output_width = 8, .output_height = 4, .output_stride = 1, .clear_background_pending = true,
        };
        memset(pixels, 0, sizeof(pixels));
        for (unsigned i = 0; i < 2; ++i)
            for (unsigned literal = 0; literal < 2; ++literal)
                for (unsigned reverse = 0; reverse < 2; ++reverse) {
                    inverted = i;
                    p.config.literal_direct_palette = literal;
                    p.config.reverse_direct_palette = reverse;
                    p.mono_fallback = false;
                    assert(frame_present(&p) == ESP_OK);
                    assert(captured.palette_inverted == (!literal && (i != reverse)));
                    assert(captured.palette_rgb565 == palette && captured.clear_background);
                    assert(captured.background_index == 33 && captured.format == format);
                    /* Mono polarity is independent of the direct palette flag. */
                    p.mono_fallback = true;
                    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8)
                        for (unsigned n = 0; n < sizeof(pixels); ++n) pixels[n] = n & 1 ? 33 : 32;
                    frame_prepare_mono_palette(&p);
                    assert(frame_present(&p) == ESP_OK);
                    assert(captured.format == SOLAR_OS_DISPLAY_FORMAT_MONO1);
                    assert(captured.palette_inverted == inverted && !captured.clear_background);
                    assert(!captured.palette_rgb565 && captured.data == mono);
                    for (unsigned row = 0; row < 4; ++row)
                        assert(mono[row] == (format == SOLAR_OS_DISPLAY_FORMAT_INDEX8 ? 0x55 : 0));
                    if (format == SOLAR_OS_DISPLAY_FORMAT_INDEX2)
                        assert(p.mono_luma[0] == 255 && p.mono_luma[3] == 0);
                }
    }
    /* A literal mono palette keeps black backgrounds and luminous eyes even
     * when the terminal theme is inverted; direct-only flags retain legacy behavior. */
    uint16_t eye_palette[4] = {0, 0, 0, 0xffff};
    uint8_t eyes[8];
    memset(eyes, 0xcc, sizeof(eyes));
    solar_os_frame_presenter_t face = {
        .config = {.gfx = (solar_os_gfx_t *)&captured, .format = SOLAR_OS_DISPLAY_FORMAT_INDEX2,
            .width = 8, .height = 4, .stride = 2, .palette_rgb565 = eye_palette, .palette_size = 4,
            .literal_mono_palette = true},
        .presenting = eyes, .mono = mono, .source_size = sizeof(eyes), .mono_size = sizeof(mono),
        .output_width = 8, .output_height = 4, .output_stride = 1, .mono_fallback = true,
    };
    inverted = true;
    frame_prepare_mono_palette(&face);
    assert(face.mono_luma[0] == 0 && face.mono_luma[3] == 255);
    assert(frame_present(&face) == ESP_OK && !captured.palette_inverted);
    for (unsigned row = 0; row < 4; ++row) assert(mono[row] == 0x55);
    puts("Literal color palettes, INDEX2 compatibility and mono polarity tests passed");
    return 0;
}
