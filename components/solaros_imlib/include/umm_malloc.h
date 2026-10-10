/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2007-2017 Ralph Hempel
 * Copyright (C) 2017-2024 OpenMV, LLC.
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
 * UMM memory allocator interface.
 */

#ifndef ESP_VISION_IMLIB_UMM_MALLOC_H
#define ESP_VISION_IMLIB_UMM_MALLOC_H

#include <stddef.h>

void umm_alloc_fail(void);
void umm_init_x(size_t size);
void umm_deinit_x(void);
void umm_alloc_mark(void);
void umm_alloc_free_till_mark(void);
void *umm_malloc(size_t size);
void *umm_calloc(size_t num, size_t size);
void *umm_realloc(void *ptr, size_t size);
void umm_free(void *ptr);

#endif /* ESP_VISION_IMLIB_UMM_MALLOC_H */
