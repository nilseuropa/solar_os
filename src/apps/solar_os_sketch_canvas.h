#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

typedef struct {
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
} solar_os_sketch_canvas_t;

extern const uint32_t solar_os_sketch_palette_rgb888[4];

size_t solar_os_sketch_canvas_bytes(uint16_t width, uint16_t height);
size_t solar_os_sketch_canvas_xbm_bytes(uint16_t width, uint16_t height);
size_t solar_os_sketch_canvas_fill_workspace_bytes(
    const solar_os_sketch_canvas_t *canvas);
void solar_os_sketch_canvas_clear(solar_os_sketch_canvas_t *canvas,
                                  uint8_t color);
uint8_t solar_os_sketch_canvas_get(const solar_os_sketch_canvas_t *canvas,
                                   int x,
                                   int y);
void solar_os_sketch_canvas_set(solar_os_sketch_canvas_t *canvas,
                                int x,
                                int y,
                                uint8_t color,
                                uint8_t pattern);
void solar_os_sketch_canvas_line(solar_os_sketch_canvas_t *canvas,
                                 int x0,
                                 int y0,
                                 int x1,
                                 int y1,
                                 uint8_t color,
                                 uint8_t pattern,
                                 uint8_t weight);
void solar_os_sketch_canvas_rect(solar_os_sketch_canvas_t *canvas,
                                 int x0,
                                 int y0,
                                 int x1,
                                 int y1,
                                 uint8_t color,
                                 uint8_t pattern,
                                 uint8_t weight);
void solar_os_sketch_canvas_ellipse(solar_os_sketch_canvas_t *canvas,
                                    int x0,
                                    int y0,
                                    int x1,
                                    int y1,
                                    uint8_t color,
                                    uint8_t pattern,
                                    uint8_t weight);
bool solar_os_sketch_canvas_flood_fill(solar_os_sketch_canvas_t *canvas,
                                       int x,
                                       int y,
                                       uint8_t color,
                                       uint8_t pattern,
                                       uint8_t *workspace,
                                       size_t workspace_size);
bool solar_os_sketch_canvas_render_xbm(
    const solar_os_sketch_canvas_t *canvas,
    uint8_t *bitmap,
    size_t bitmap_size);

/* Write an interoperable 8-bit grayscale PNG with filter-free, uncompressed
 * DEFLATE rows. The caller owns flushing, syncing, and atomic replacement. */
esp_err_t solar_os_sketch_canvas_write_png(
    FILE *file,
    const solar_os_sketch_canvas_t *canvas);
