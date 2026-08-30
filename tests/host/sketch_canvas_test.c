#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_sketch_canvas.h"

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
        ((uint32_t)data[2] << 8U) | data[3];
}

int main(void)
{
    uint8_t pixels[solar_os_sketch_canvas_bytes(3U, 2U)];
    solar_os_sketch_canvas_t canvas = {
        .pixels = pixels,
        .width = 3U,
        .height = 2U,
    };
    solar_os_sketch_canvas_clear(&canvas, 0U);
    assert(solar_os_sketch_canvas_get(&canvas, 2, 1) == 0U);
    solar_os_sketch_canvas_set(&canvas, 0, 0, 3U, 0U);
    solar_os_sketch_canvas_set(&canvas, 1, 0, 2U, 0U);
    solar_os_sketch_canvas_set(&canvas, 2, 0, 1U, 0U);
    assert(solar_os_sketch_canvas_get(&canvas, 0, 0) == 3U);
    assert(solar_os_sketch_canvas_get(&canvas, 1, 0) == 2U);
    assert(solar_os_sketch_canvas_get(&canvas, 2, 0) == 1U);
    uint8_t xbm[solar_os_sketch_canvas_xbm_bytes(3U, 2U)];
    assert(solar_os_sketch_canvas_render_xbm(&canvas, xbm, sizeof(xbm)));
    assert((xbm[0] & 0x03U) == 0x03U);
    assert((xbm[0] & 0x04U) == 0U);
    assert(xbm[1] == 0U);

    FILE *file = tmpfile();
    assert(file != NULL);
    assert(solar_os_sketch_canvas_write_png(file, &canvas) == ESP_OK);
    assert(fflush(file) == 0);
    assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t png[256];
    const size_t length = fread(png, 1, sizeof(png), file);
    assert(fclose(file) == 0);
    static const uint8_t signature[8] = {
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    };
    assert(length > 60U);
    assert(memcmp(png, signature, sizeof(signature)) == 0);
    assert(read_be32(&png[8]) == 13U);
    assert(memcmp(&png[12], "IHDR", 4) == 0);
    assert(read_be32(&png[16]) == 3U);
    assert(read_be32(&png[20]) == 2U);
    assert(png[24] == 8U && png[25] == 0U);

    assert(memcmp(&png[37], "IDAT", 4) == 0);
    assert(png[41] == 0x78U && png[42] == 0x01U);
    assert(png[43] == 1U);
    const uint16_t raw_length = (uint16_t)png[44] |
        ((uint16_t)png[45] << 8U);
    assert(raw_length == 8U);
    assert(png[48] == 0U);
    assert(png[49] == 0U);
    assert(png[50] == 85U);
    assert(png[51] == 170U);
    assert(png[52] == 0U);
    assert(png[53] == 255U && png[54] == 255U && png[55] == 255U);

    uint8_t large_pixels[solar_os_sketch_canvas_bytes(256U, 256U)];
    solar_os_sketch_canvas_t large = {
        .pixels = large_pixels,
        .width = 256U,
        .height = 256U,
    };
    solar_os_sketch_canvas_clear(&large, 2U);
    file = tmpfile();
    assert(file != NULL);
    assert(solar_os_sketch_canvas_write_png(file, &large) == ESP_OK);
    assert(ftell(file) > 65535L);
    assert(fclose(file) == 0);

    uint8_t drawing_pixels[solar_os_sketch_canvas_bytes(20U, 20U)];
    solar_os_sketch_canvas_t drawing = {
        .pixels = drawing_pixels,
        .width = 20U,
        .height = 20U,
    };
    solar_os_sketch_canvas_clear(&drawing, 0U);
    solar_os_sketch_canvas_line(&drawing, 0, 0, 19, 19, 3U, 0U, 1U);
    solar_os_sketch_canvas_rect(&drawing, 2, 3, 10, 12, 2U, 0U, 1U);
    solar_os_sketch_canvas_ellipse(&drawing, 4, 4, 16, 14, 1U, 0U, 1U);
    assert(solar_os_sketch_canvas_get(&drawing, 0, 0) == 3U);
    assert(solar_os_sketch_canvas_get(&drawing, 2, 3) == 2U);
    assert(solar_os_sketch_canvas_get(&drawing, 10, 12) == 2U);
    assert(solar_os_sketch_canvas_get(&drawing, 10, 4) == 1U);

    uint8_t fill_pixels[solar_os_sketch_canvas_bytes(8U, 6U)];
    solar_os_sketch_canvas_t fill = {
        .pixels = fill_pixels,
        .width = 8U,
        .height = 6U,
    };
    uint8_t fill_workspace[8U];
    solar_os_sketch_canvas_clear(&fill, 0U);
    solar_os_sketch_canvas_line(&fill, 4, 0, 4, 5, 3U, 0U, 1U);
    assert(solar_os_sketch_canvas_flood_fill(
        &fill, 1, 1, 2U, 0U, fill_workspace, sizeof(fill_workspace)));
    assert(solar_os_sketch_canvas_get(&fill, 1, 1) == 2U);
    assert(solar_os_sketch_canvas_get(&fill, 4, 1) == 3U);
    assert(solar_os_sketch_canvas_get(&fill, 6, 1) == 0U);
    assert(solar_os_sketch_canvas_flood_fill(
        &fill, 6, 1, 1U, 1U, fill_workspace, sizeof(fill_workspace)));
    assert(solar_os_sketch_canvas_get(&fill, 6, 2) == 1U);
    assert(solar_os_sketch_canvas_get(&fill, 6, 1) == 0U);

    puts("sketch canvas and PNG tests: ok");
    return 0;
}
