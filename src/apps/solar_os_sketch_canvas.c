#include "solar_os_sketch_canvas.h"

#include <stdbool.h>
#include <string.h>

const uint32_t solar_os_sketch_palette_rgb888[4] = {
    0xffffffU,
    0xe53935U,
    0x1e63d5U,
    0x000000U,
};

size_t solar_os_sketch_canvas_bytes(uint16_t width, uint16_t height)
{
    return ((size_t)width * (size_t)height + 3U) / 4U;
}

size_t solar_os_sketch_canvas_xbm_bytes(uint16_t width, uint16_t height)
{
    return (((size_t)width + 7U) / 8U) * (size_t)height;
}

size_t solar_os_sketch_canvas_fill_workspace_bytes(
    const solar_os_sketch_canvas_t *canvas)
{
    return canvas != NULL ?
        (((size_t)canvas->width * (size_t)canvas->height + 7U) / 8U) : 0U;
}

static bool sketch_canvas_contains(const solar_os_sketch_canvas_t *canvas,
                                   int x,
                                   int y)
{
    return canvas != NULL && canvas->pixels != NULL && x >= 0 && y >= 0 &&
        x < (int)canvas->width && y < (int)canvas->height;
}

void solar_os_sketch_canvas_clear(solar_os_sketch_canvas_t *canvas,
                                  uint8_t color)
{
    if (canvas == NULL || canvas->pixels == NULL) {
        return;
    }
    color &= 3U;
    const uint8_t packed = (uint8_t)(color | (color << 2U) |
                                     (color << 4U) | (color << 6U));
    memset(canvas->pixels, packed,
           solar_os_sketch_canvas_bytes(canvas->width, canvas->height));
}

uint8_t solar_os_sketch_canvas_get(const solar_os_sketch_canvas_t *canvas,
                                   int x,
                                   int y)
{
    if (!sketch_canvas_contains(canvas, x, y)) {
        return 0U;
    }
    const size_t pixel = (size_t)y * canvas->width + (size_t)x;
    return (uint8_t)((canvas->pixels[pixel >> 2U] >>
                      ((pixel & 3U) * 2U)) & 3U);
}

static bool sketch_pattern_accepts(uint8_t pattern, int x, int y)
{
    switch (pattern & 3U) {
    case 1U:
        return ((x + y) & 1) == 0;
    case 2U:
        return (x & 3) == 0 && (y & 3) == 0;
    case 3U:
        return ((x + y) & 3) == 0;
    default:
        return true;
    }
}

void solar_os_sketch_canvas_set(solar_os_sketch_canvas_t *canvas,
                                int x,
                                int y,
                                uint8_t color,
                                uint8_t pattern)
{
    if (!sketch_canvas_contains(canvas, x, y) ||
        !sketch_pattern_accepts(pattern, x, y)) {
        return;
    }
    const size_t pixel = (size_t)y * canvas->width + (size_t)x;
    const unsigned shift = (unsigned)((pixel & 3U) * 2U);
    const uint8_t mask = (uint8_t)(3U << shift);
    canvas->pixels[pixel >> 2U] =
        (uint8_t)((canvas->pixels[pixel >> 2U] & (uint8_t)~mask) |
                  ((color & 3U) << shift));
}

static void sketch_brush(solar_os_sketch_canvas_t *canvas,
                         int x,
                         int y,
                         uint8_t color,
                         uint8_t pattern,
                         uint8_t weight)
{
    const int radius = weight > 1U ? (int)weight / 2 : 0;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            if (radius == 0 || dx * dx + dy * dy <= radius * radius + 1) {
                solar_os_sketch_canvas_set(canvas, x + dx, y + dy,
                                           color, pattern);
            }
        }
    }
}

void solar_os_sketch_canvas_line(solar_os_sketch_canvas_t *canvas,
                                 int x0,
                                 int y0,
                                 int x1,
                                 int y1,
                                 uint8_t color,
                                 uint8_t pattern,
                                 uint8_t weight)
{
    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    const int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        sketch_brush(canvas, x0, y0, color, pattern, weight);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void solar_os_sketch_canvas_rect(solar_os_sketch_canvas_t *canvas,
                                 int x0,
                                 int y0,
                                 int x1,
                                 int y1,
                                 uint8_t color,
                                 uint8_t pattern,
                                 uint8_t weight)
{
    if (x0 > x1) {
        const int swap = x0;
        x0 = x1;
        x1 = swap;
    }
    if (y0 > y1) {
        const int swap = y0;
        y0 = y1;
        y1 = swap;
    }
    solar_os_sketch_canvas_line(canvas, x0, y0, x1, y0,
                                color, pattern, weight);
    solar_os_sketch_canvas_line(canvas, x1, y0, x1, y1,
                                color, pattern, weight);
    solar_os_sketch_canvas_line(canvas, x1, y1, x0, y1,
                                color, pattern, weight);
    solar_os_sketch_canvas_line(canvas, x0, y1, x0, y0,
                                color, pattern, weight);
}

static void sketch_ellipse_points(solar_os_sketch_canvas_t *canvas,
                                  int center_x,
                                  int center_y,
                                  int x,
                                  int y,
                                  uint8_t color,
                                  uint8_t pattern,
                                  uint8_t weight)
{
    sketch_brush(canvas, center_x + x, center_y + y,
                 color, pattern, weight);
    sketch_brush(canvas, center_x - x, center_y + y,
                 color, pattern, weight);
    sketch_brush(canvas, center_x + x, center_y - y,
                 color, pattern, weight);
    sketch_brush(canvas, center_x - x, center_y - y,
                 color, pattern, weight);
}

void solar_os_sketch_canvas_ellipse(solar_os_sketch_canvas_t *canvas,
                                    int x0,
                                    int y0,
                                    int x1,
                                    int y1,
                                    uint8_t color,
                                    uint8_t pattern,
                                    uint8_t weight)
{
    if (x0 > x1) {
        const int swap = x0;
        x0 = x1;
        x1 = swap;
    }
    if (y0 > y1) {
        const int swap = y0;
        y0 = y1;
        y1 = swap;
    }
    const int rx = (x1 - x0) / 2;
    const int ry = (y1 - y0) / 2;
    const int cx = x0 + rx;
    const int cy = y0 + ry;
    if (rx == 0 || ry == 0) {
        solar_os_sketch_canvas_line(canvas, x0, y0, x1, y1,
                                    color, pattern, weight);
        return;
    }

    int x = 0;
    int y = ry;
    const int64_t rx2 = (int64_t)rx * rx;
    const int64_t ry2 = (int64_t)ry * ry;
    int64_t dx = 0;
    int64_t dy = 2 * rx2 * y;
    int64_t decision = ry2 - rx2 * ry + rx2 / 4;
    while (dx < dy) {
        sketch_ellipse_points(canvas, cx, cy, x, y,
                              color, pattern, weight);
        x++;
        dx += 2 * ry2;
        if (decision < 0) {
            decision += dx + ry2;
        } else {
            y--;
            dy -= 2 * rx2;
            decision += dx - dy + ry2;
        }
    }
    decision = ry2 * (int64_t)(x * x + x) + ry2 / 4 +
        rx2 * (int64_t)(y - 1) * (y - 1) - rx2 * ry2;
    while (y >= 0) {
        sketch_ellipse_points(canvas, cx, cy, x, y,
                              color, pattern, weight);
        y--;
        dy -= 2 * rx2;
        if (decision > 0) {
            decision += rx2 - dy;
        } else {
            x++;
            dx += 2 * ry2;
            decision += dx - dy + rx2;
        }
    }
}

static bool sketch_workspace_get(const uint8_t *workspace, size_t pixel)
{
    return (workspace[pixel >> 3U] & (uint8_t)(1U << (pixel & 7U))) != 0U;
}

static void sketch_workspace_set(uint8_t *workspace, size_t pixel)
{
    workspace[pixel >> 3U] |= (uint8_t)(1U << (pixel & 7U));
}

bool solar_os_sketch_canvas_flood_fill(solar_os_sketch_canvas_t *canvas,
                                       int x,
                                       int y,
                                       uint8_t color,
                                       uint8_t pattern,
                                       uint8_t *workspace,
                                       size_t workspace_size)
{
    if (!sketch_canvas_contains(canvas, x, y) || workspace == NULL ||
        workspace_size < solar_os_sketch_canvas_fill_workspace_bytes(canvas)) {
        return false;
    }
    const uint8_t target = solar_os_sketch_canvas_get(canvas, x, y);
    if ((color & 3U) == target) {
        return false;
    }
    memset(workspace, 0, solar_os_sketch_canvas_fill_workspace_bytes(canvas));
    sketch_workspace_set(workspace, (size_t)y * canvas->width + (size_t)x);

    bool expanded;
    do {
        expanded = false;
        for (int row = 0; row < (int)canvas->height; row++) {
            for (int column = 0; column < (int)canvas->width; column++) {
                const size_t pixel = (size_t)row * canvas->width +
                    (size_t)column;
                if (sketch_workspace_get(workspace, pixel) ||
                    solar_os_sketch_canvas_get(canvas, column, row) != target) {
                    continue;
                }
                if ((column > 0 &&
                     sketch_workspace_get(workspace, pixel - 1U)) ||
                    (row > 0 && sketch_workspace_get(
                        workspace, pixel - canvas->width))) {
                    sketch_workspace_set(workspace, pixel);
                    expanded = true;
                }
            }
        }
        for (int row = (int)canvas->height - 1; row >= 0; row--) {
            for (int column = (int)canvas->width - 1; column >= 0; column--) {
                const size_t pixel = (size_t)row * canvas->width +
                    (size_t)column;
                if (sketch_workspace_get(workspace, pixel) ||
                    solar_os_sketch_canvas_get(canvas, column, row) != target) {
                    continue;
                }
                if ((column + 1 < (int)canvas->width &&
                     sketch_workspace_get(workspace, pixel + 1U)) ||
                    (row + 1 < (int)canvas->height && sketch_workspace_get(
                        workspace, pixel + canvas->width))) {
                    sketch_workspace_set(workspace, pixel);
                    expanded = true;
                }
            }
        }
    } while (expanded);

    const size_t pixels = (size_t)canvas->width * canvas->height;
    for (size_t pixel = 0; pixel < pixels; pixel++) {
        if (sketch_workspace_get(workspace, pixel)) {
            solar_os_sketch_canvas_set(canvas,
                                       (int)(pixel % canvas->width),
                                       (int)(pixel / canvas->width),
                                       color,
                                       pattern);
        }
    }
    return true;
}

bool solar_os_sketch_canvas_render_xbm(
    const solar_os_sketch_canvas_t *canvas,
    uint8_t *bitmap,
    size_t bitmap_size)
{
    static const uint8_t bayer4[4][4] = {
        {0U, 8U, 2U, 10U},
        {12U, 4U, 14U, 6U},
        {3U, 11U, 1U, 9U},
        {15U, 7U, 13U, 5U},
    };
    const size_t required = canvas != NULL ?
        solar_os_sketch_canvas_xbm_bytes(canvas->width, canvas->height) : 0U;
    if (canvas == NULL || canvas->pixels == NULL || bitmap == NULL ||
        required == 0U || bitmap_size < required) {
        return false;
    }
    const size_t stride = ((size_t)canvas->width + 7U) / 8U;
    memset(bitmap, 0, required);
    for (int y_pos = 0; y_pos < (int)canvas->height; y_pos++) {
        for (int x_pos = 0; x_pos < (int)canvas->width; x_pos++) {
            const uint8_t shade = solar_os_sketch_canvas_get(
                canvas, x_pos, y_pos);
            const uint8_t threshold = shade == 0U ? 16U :
                (shade == 1U ? 12U : (shade == 2U ? 5U : 0U));
            if (bayer4[y_pos & 3][x_pos & 3] >= threshold) {
                bitmap[(size_t)y_pos * stride + ((size_t)x_pos >> 3U)] |=
                    (uint8_t)(1U << (x_pos & 7));
            }
        }
    }
    return true;
}

static uint32_t sketch_crc32_update(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (unsigned bit = 0; bit < 8U; bit++) {
        crc = (crc >> 1U) ^ (0xedb88320U &
                             (uint32_t)-(int32_t)(crc & 1U));
    }
    return crc;
}

static bool sketch_write_byte(FILE *file, uint8_t value)
{
    return fputc(value, file) != EOF;
}

static bool sketch_write_be32(FILE *file, uint32_t value)
{
    return sketch_write_byte(file, (uint8_t)(value >> 24U)) &&
        sketch_write_byte(file, (uint8_t)(value >> 16U)) &&
        sketch_write_byte(file, (uint8_t)(value >> 8U)) &&
        sketch_write_byte(file, (uint8_t)value);
}

static bool sketch_chunk_begin(FILE *file,
                               uint32_t length,
                               const char type[4],
                               uint32_t *crc)
{
    if (!sketch_write_be32(file, length) || fwrite(type, 1, 4, file) != 4U) {
        return false;
    }
    *crc = 0xffffffffU;
    for (size_t index = 0; index < 4U; index++) {
        *crc = sketch_crc32_update(*crc, (uint8_t)type[index]);
    }
    return true;
}

static bool sketch_chunk_byte(FILE *file, uint8_t byte, uint32_t *crc)
{
    if (!sketch_write_byte(file, byte)) {
        return false;
    }
    *crc = sketch_crc32_update(*crc, byte);
    return true;
}

static bool sketch_chunk_end(FILE *file, uint32_t crc)
{
    return sketch_write_be32(file, crc ^ 0xffffffffU);
}

static uint8_t sketch_png_sample(const solar_os_sketch_canvas_t *canvas,
                                 size_t raw_offset)
{
    const size_t row_bytes = ((size_t)canvas->width + 3U) / 4U;
    const size_t stride = row_bytes + 1U;
    const size_t column = raw_offset % stride;
    if (column == 0U) {
        return 0U;
    }
    const int y = (int)(raw_offset / stride);
    const int first_x = (int)((column - 1U) * 4U);
    uint8_t packed = 0U;
    for (int pixel = 0; pixel < 4; pixel++) {
        const int x = first_x + pixel;
        const uint8_t value = x < (int)canvas->width ?
            solar_os_sketch_canvas_get(canvas, x, y) : 0U;
        packed |= (uint8_t)(value << (6 - pixel * 2));
    }
    return packed;
}

esp_err_t solar_os_sketch_canvas_write_png(
    FILE *file,
    const solar_os_sketch_canvas_t *canvas)
{
    static const uint8_t signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    };
    if (file == NULL || canvas == NULL || canvas->pixels == NULL ||
        canvas->width == 0U || canvas->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (fwrite(signature, 1, sizeof(signature), file) != sizeof(signature)) {
        return ESP_FAIL;
    }

    uint32_t crc = 0;
    if (!sketch_chunk_begin(file, 13U, "IHDR", &crc)) {
        return ESP_FAIL;
    }
    const uint8_t ihdr[13] = {
        0U, 0U, (uint8_t)(canvas->width >> 8U), (uint8_t)canvas->width,
        0U, 0U, (uint8_t)(canvas->height >> 8U), (uint8_t)canvas->height,
        2U, 3U, 0U, 0U, 0U,
    };
    for (size_t index = 0; index < sizeof(ihdr); index++) {
        if (!sketch_chunk_byte(file, ihdr[index], &crc)) {
            return ESP_FAIL;
        }
    }
    if (!sketch_chunk_end(file, crc)) {
        return ESP_FAIL;
    }

    if (!sketch_chunk_begin(file, 12U, "PLTE", &crc)) {
        return ESP_FAIL;
    }
    for (size_t index = 0; index < 4U; index++) {
        const uint32_t rgb = solar_os_sketch_palette_rgb888[index];
        if (!sketch_chunk_byte(file, (uint8_t)(rgb >> 16U), &crc) ||
            !sketch_chunk_byte(file, (uint8_t)(rgb >> 8U), &crc) ||
            !sketch_chunk_byte(file, (uint8_t)rgb, &crc)) {
            return ESP_FAIL;
        }
    }
    if (!sketch_chunk_end(file, crc)) {
        return ESP_FAIL;
    }

    const size_t raw_len = ((((size_t)canvas->width + 3U) / 4U) + 1U) *
        canvas->height;
    const size_t blocks = raw_len / 65535U +
        (raw_len % 65535U != 0U ? 1U : 0U);
    const uint64_t idat_len_size = 2U + (uint64_t)raw_len +
        (uint64_t)blocks * 5U + 4U;
    if (idat_len_size > UINT32_MAX ||
        !sketch_chunk_begin(file, (uint32_t)idat_len_size, "IDAT", &crc) ||
        !sketch_chunk_byte(file, 0x78U, &crc) ||
        !sketch_chunk_byte(file, 0x01U, &crc)) {
        return ESP_FAIL;
    }

    uint32_t adler_a = 1U;
    uint32_t adler_b = 0U;
    size_t raw_offset = 0U;
    while (raw_offset < raw_len) {
        const size_t remaining = raw_len - raw_offset;
        const uint16_t length =
            (uint16_t)(remaining > 65535U ? 65535U : remaining);
        const uint16_t inverse = (uint16_t)~length;
        const uint8_t final = remaining <= 65535U ? 1U : 0U;
        if (!sketch_chunk_byte(file, final, &crc) ||
            !sketch_chunk_byte(file, (uint8_t)length, &crc) ||
            !sketch_chunk_byte(file, (uint8_t)(length >> 8U), &crc) ||
            !sketch_chunk_byte(file, (uint8_t)inverse, &crc) ||
            !sketch_chunk_byte(file, (uint8_t)(inverse >> 8U), &crc)) {
            return ESP_FAIL;
        }
        for (size_t index = 0; index < (size_t)length;
             index++, raw_offset++) {
            const uint8_t byte = sketch_png_sample(canvas, raw_offset);
            if (!sketch_chunk_byte(file, byte, &crc)) {
                return ESP_FAIL;
            }
            adler_a = (adler_a + byte) % 65521U;
            adler_b = (adler_b + adler_a) % 65521U;
        }
    }
    const uint32_t adler = (adler_b << 16U) | adler_a;
    for (int shift = 24; shift >= 0; shift -= 8) {
        if (!sketch_chunk_byte(file, (uint8_t)(adler >> shift), &crc)) {
            return ESP_FAIL;
        }
    }
    if (!sketch_chunk_end(file, crc) ||
        !sketch_chunk_begin(file, 0U, "IEND", &crc) ||
        !sketch_chunk_end(file, crc)) {
        return ESP_FAIL;
    }
    return ferror(file) == 0 ? ESP_OK : ESP_FAIL;
}
