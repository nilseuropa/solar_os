/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2013-2024 OpenMV, LLC.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Image library.
 */
/* Selected helpers from imlib.c; see README.solaros.md. */
#include "imlib.h"

int imlib_ksize_to_n(int ksize) {
    return ((ksize * 2) + 1) * ((ksize * 2) + 1);
}

void rectangle_init(rectangle_t *ptr, int x, int y, int w, int h) {
    ptr->x = x;
    ptr->y = y;
    ptr->w = w;
    ptr->h = h;
}

bool rectangle_overlap(rectangle_t *ptr0, rectangle_t *ptr1) {
    int x0 = ptr0->x;
    int y0 = ptr0->y;
    int w0 = ptr0->w;
    int h0 = ptr0->h;
    int x1 = ptr1->x;
    int y1 = ptr1->y;
    int w1 = ptr1->w;
    int h1 = ptr1->h;
    return (x0 < (x1 + w1)) && (y0 < (y1 + h1)) && (x1 < (x0 + w0)) && (y1 < (y0 + h0));
}

void rectangle_intersected(rectangle_t *dst, rectangle_t *src) {
    int leftX = IM_MAX(dst->x, src->x);
    int topY = IM_MAX(dst->y, src->y);
    int rightX = IM_MIN(dst->x + dst->w, src->x + src->w);
    int bottomY = IM_MIN(dst->y + dst->h, src->y + src->h);
    dst->x = leftX;
    dst->y = topY;
    dst->w = rightX - leftX;
    dst->h = bottomY - topY;
}

void rectangle_united(rectangle_t *dst, rectangle_t *src) {
    int leftX = IM_MIN(dst->x, src->x);
    int topY = IM_MIN(dst->y, src->y);
    int rightX = IM_MAX(dst->x + dst->w, src->x + src->w);
    int bottomY = IM_MAX(dst->y + dst->h, src->y + src->h);
    dst->x = leftX;
    dst->y = topY;
    dst->w = rightX - leftX;
    dst->h = bottomY - topY;
}

size_t image_line_size(image_t *ptr) {
    switch (ptr->pixfmt) {
        case PIXFORMAT_BINARY: {
            return IMAGE_BINARY_LINE_LEN_BYTES(ptr);
        }
        case PIXFORMAT_GRAYSCALE:
        case PIXFORMAT_BAYER_ANY: {
            // re-use
            return IMAGE_GRAYSCALE_LINE_LEN_BYTES(ptr);
        }
        case PIXFORMAT_RGB565:
        case PIXFORMAT_YUV_ANY: {
            // re-use
            return IMAGE_RGB565_LINE_LEN_BYTES(ptr);
        }
        default: {
            return 0;
        }
    }
}

size_t image_size(image_t *ptr) {
    switch (ptr->pixfmt) {
        case PIXFORMAT_BINARY: {
            return IMAGE_BINARY_LINE_LEN_BYTES(ptr) * ptr->h;
        }
        case PIXFORMAT_GRAYSCALE:
        case PIXFORMAT_BAYER_ANY: {
            // re-use
            return IMAGE_GRAYSCALE_LINE_LEN_BYTES(ptr) * ptr->h;
        }
        case PIXFORMAT_RGB565:
        case PIXFORMAT_YUV_ANY: {
            // re-use
            return IMAGE_RGB565_LINE_LEN_BYTES(ptr) * ptr->h;
        }
        case PIXFORMAT_COMPRESSED_ANY: {
            return ptr->size;
        }
        default: {
            return 0;
        }
    }
}

bool image_get_mask_pixel(image_t *ptr, int x, int y) {
    if ((0 <= x) && (x < ptr->w) && (0 <= y) && (y < ptr->h)) {
        switch (ptr->pixfmt) {
            case PIXFORMAT_BINARY: {
                return IMAGE_GET_BINARY_PIXEL(ptr, x, y);
            }
            case PIXFORMAT_GRAYSCALE: {
                return COLOR_GRAYSCALE_TO_BINARY(IMAGE_GET_GRAYSCALE_PIXEL(ptr, x, y));
            }
            case PIXFORMAT_RGB565: {
                return COLOR_RGB565_TO_BINARY(IMAGE_GET_RGB565_PIXEL(ptr, x, y));
            }
            default: {
                return false;
            }
        }
    }

    return false;
}
