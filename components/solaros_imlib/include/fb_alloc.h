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
 * Interface for using extra frame buffer RAM as a stack.
 */

#ifndef ESP_VISION_IMLIB_FB_ALLOC_H
#define ESP_VISION_IMLIB_FB_ALLOC_H

#include <stdint.h>

#define FB_ALLOC_NO_HINT         0
#define FB_ALLOC_PREFER_SPEED    1
#define FB_ALLOC_PREFER_SIZE     2
#define FB_ALLOC_CACHE_ALIGN     4
#define FB_ALLOC_PREFER_INTERNAL 8

#ifndef OMV_ALLOC_ALIGNMENT
#define OMV_ALLOC_ALIGNMENT     (OMV_CACHE_LINE_SIZE)
#endif

char *fb_alloc_sp(void);
void fb_alloc_fail(void);
void fb_alloc_init0(void);
uint32_t fb_avail(void);
void fb_alloc_mark(void);
void fb_alloc_free_till_mark(void);
void fb_alloc_mark_permanent(void);
void fb_alloc_free_till_mark_past_mark_permanent(void);
void *fb_alloc(uint32_t size, int hints);
void *fb_alloc0(uint32_t size, int hints);
void *fb_alloc_all(uint32_t *size, int hints);
void *fb_alloc0_all(uint32_t *size, int hints);
void fb_free(void);
void fb_free_all(void);

#endif /* ESP_VISION_IMLIB_FB_ALLOC_H */
