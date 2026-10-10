#include "solar_os_imlib.h"
#include <math.h>
#include <setjmp.h>
#include <string.h>
#include "imlib.h"
#include "esp_timer.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define IMLIB_WORKER_STACK (12U * 1024U)
#define IMLIB_WORKSPACE_MAX (2U * 1024U * 1024U)
#define IMLIB_FLOOD_STACK (128U * 1024U)
#define IMLIB_CANDIDATES_MAX 512U

typedef struct imlib_allocation {
    struct imlib_allocation *prev, *next, *fb_prev;
    void *data;
    size_t bytes;
    bool mark;
} imlib_allocation_t;
typedef struct {
    solar_os_raster_image_t *source, *reference;
    solar_os_imlib_options_t options;
    solar_os_imlib_result_t *result;
    imlib_allocation_t *allocations, *fb_head;
    size_t allocated, peak, candidates;
    int64_t deadline_us, yield_us;
    esp_err_t error;
    jmp_buf unwind;
    bool done, cancelled;
} imlib_work_t;
static bool imlib_busy;
/* Only the admitted worker accesses this pointer. Never an interpreter or ISR. */
static imlib_work_t *imlib_active;

static void imlib_fail(esp_err_t error)
{
    imlib_active->error = error;
    longjmp(imlib_active->unwind, 1);
}
void solar_os_imlib_poll(void)
{
    imlib_work_t *w = imlib_active;
    int64_t now = esp_timer_get_time();
    if (__atomic_load_n(&w->cancelled, __ATOMIC_ACQUIRE) || now >= w->deadline_us)
        imlib_fail(ESP_ERR_TIMEOUT);
    if (now - w->yield_us >= 2000) { w->yield_us = now; vTaskDelay(1); }
}
void *solar_os_imlib_malloc(size_t bytes)
{
    if (!bytes) return NULL;
    imlib_work_t *w = imlib_active;
    solar_os_imlib_poll();
    if (bytes > IMLIB_WORKSPACE_MAX - w->allocated ||
        sizeof(imlib_allocation_t) + 31 > IMLIB_WORKSPACE_MAX - w->allocated - bytes)
        imlib_fail(ESP_ERR_NO_MEM);
    size_t total = sizeof(imlib_allocation_t) + 31 + bytes;
    imlib_allocation_t *node = solar_os_memory_alloc(total,
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "imlib.workspace");
    if (!node) imlib_fail(ESP_ERR_NO_MEM);
    *node = (imlib_allocation_t){.next = w->allocations, .bytes = total,
        .data = (void *)(((uintptr_t)(node + 1) + 31) & ~(uintptr_t)31)};
    if (node->next) node->next->prev = node;
    w->allocations = node; w->allocated += total;
    if (w->allocated > w->peak) w->peak = w->allocated;
    return node->data;
}
void *solar_os_imlib_calloc(size_t count, size_t bytes)
{
    if (bytes && count > SIZE_MAX / bytes) imlib_fail(ESP_ERR_NO_MEM);
    void *p = solar_os_imlib_malloc(count * bytes);
    if (p) memset(p, 0, count * bytes);
    return p;
}
void solar_os_imlib_free(void *p)
{
    if (!p) return;
    imlib_work_t *w = imlib_active;
    for (imlib_allocation_t *n = w->allocations; n; n = n->next) {
        if (n->data != p) continue;
        if (n->prev) n->prev->next = n->next; else w->allocations = n->next;
        if (n->next) n->next->prev = n->prev;
        w->allocated -= n->bytes; solar_os_memory_free(n); return;
    }
    imlib_fail(ESP_ERR_INVALID_STATE);
}
/* The selected upstream kernels use a LIFO framebuffer allocator and ordinary
 * linked-list allocations. Both are tracked by the same request for unwind. */
void *fb_alloc(uint32_t size, int hints)
{
    (void)hints;
    void *p = solar_os_imlib_malloc(size);
    if (p) { imlib_active->allocations->fb_prev = imlib_active->fb_head;
        imlib_active->fb_head = imlib_active->allocations; }
    return p;
}
void *fb_alloc0(uint32_t size, int hints)
{ void *p = fb_alloc(size, hints); if (p) memset(p, 0, size); return p; }
void fb_free(void)
{
    imlib_allocation_t *n = imlib_active->fb_head;
    if (n) { imlib_active->fb_head = n->fb_prev; solar_os_imlib_free(n->data); }
}
void fb_alloc_mark(void)
{ (void)fb_alloc(1, 0); imlib_active->fb_head->mark = true; }
void fb_alloc_mark_permanent(void) { }
void fb_alloc_free_till_mark(void)
{
    while (imlib_active->fb_head) {
        bool mark = imlib_active->fb_head->mark; fb_free(); if (mark) break;
    }
}
void fb_alloc_free_till_mark_past_mark_permanent(void) { fb_alloc_free_till_mark(); }
void fb_alloc_fail(void) { imlib_fail(ESP_ERR_NO_MEM); }
uint32_t fb_avail(void) { return IMLIB_WORKSPACE_MAX - imlib_active->allocated; }
void *fb_alloc_all(uint32_t *size, int hints)
{ *size = IMLIB_FLOOD_STACK; return fb_alloc(*size, hints); }

static const char *const operations[] = {
    "histogram", "statistics", "binary", "invert", "mean", "gaussian", "median",
    "erode", "dilate", "opening", "closing", "difference", "blobs",
};
const char *solar_os_imlib_operation_name(solar_os_imlib_operation_t op)
{ return (unsigned)op < SOLAR_OS_IMLIB_OPERATIONS_COUNT ? operations[op] : NULL; }
bool solar_os_imlib_is_transform(solar_os_imlib_operation_t op)
{ return op >= SOLAR_OS_IMLIB_BINARY && op <= SOLAR_OS_IMLIB_DIFFERENCE; }
bool solar_os_imlib_option_allowed(solar_os_imlib_operation_t op, const char *key)
{
    if (!solar_os_imlib_operation_name(op) || !key) return false;
    const char *const common[] = {"format", "x", "y", "width", "height",
        "output_width", "output_height", "timeout_ms"};
    for (size_t i = 0; i < sizeof(common)/sizeof(common[0]); ++i)
        if (!strcmp(key, common[i])) return true;
    if (!strcmp(key, "ksize")) return op >= SOLAR_OS_IMLIB_MEAN && op <= SOLAR_OS_IMLIB_CLOSING;
    if (!strcmp(key, "bins")) return op <= SOLAR_OS_IMLIB_STATISTICS;
    if (!strcmp(key, "thresholds") || !strcmp(key, "invert"))
        return op <= SOLAR_OS_IMLIB_BINARY || op == SOLAR_OS_IMLIB_BLOBS;
    if (op == SOLAR_OS_IMLIB_BLOBS) {
        const char *const keys[] = {"x_stride", "y_stride", "area_threshold",
            "pixels_threshold", "merge", "margin", "max_blobs"};
        for (size_t i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i)
            if (!strcmp(key, keys[i])) return true;
    }
    return false;
}
solar_os_imlib_options_t solar_os_imlib_default_options(void)
{
    return (solar_os_imlib_options_t){.image = {.format = SOLAR_OS_RASTER_IMAGE_GRAY8},
        .timeout_ms = 5000, .ksize = 1, .x_stride = 1, .y_stride = 1,
        .area_threshold = 10, .pixels_threshold = 10, .max_blobs = 32};
}
void solar_os_imlib_result_free(solar_os_imlib_result_t *r)
{ if (r) { solar_os_raster_image_release(r->image); solar_os_memory_free(r); } }

static image_t imlib_prepare(solar_os_raster_image_t *source,
    const solar_os_raster_image_convert_options_t *opt)
{
    image_t image = {.w = opt->output_width, .h = opt->output_height,
        .pixfmt = opt->format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? PIXFORMAT_GRAYSCALE : PIXFORMAT_RGB565};
    image.data = solar_os_imlib_malloc(image_size(&image));
    esp_err_t error = solar_os_raster_image_convert(source, opt,
        image.data, image_size(&image), 0);
    if (error) imlib_fail(error);
    return image;
}
static bool imlib_blob_candidate(void *user, find_blobs_list_lnk_data_t *blob)
{
    (void)blob;
    imlib_work_t *w = user;
    solar_os_imlib_poll();
    if (++w->candidates > IMLIB_CANDIDATES_MAX) { w->result->truncated = true; return false; }
    return true;
}
static void imlib_histogram(imlib_work_t *w, image_t *image, list_t *thresholds)
{
    solar_os_imlib_result_t *r = w->result;
    bool color = image->pixfmt == PIXFORMAT_RGB565;
    r->channels = color ? 3 : 1;
    r->bins[0] = w->options.bins ? w->options.bins : color ? 101 : 256;
    r->bins[1] = r->bins[2] = color ? (w->options.bins ? w->options.bins : 256) : 0;
    histogram_t hist = {.LBinCount = r->bins[0], .LBins = r->histogram[0],
        .ABinCount = r->bins[1], .ABins = r->histogram[1],
        .BBinCount = r->bins[2], .BBins = r->histogram[2]};
    rectangle_t roi = {.w = image->w, .h = image->h};
    imlib_get_histogram(&hist, image, &roi, thresholds, w->options.invert, NULL);
    statistics_t stats;
    imlib_get_statistics(&stats, image->pixfmt, &hist);
    r->statistics[0] = (solar_os_imlib_statistics_t){stats.LMean, stats.LMedian, stats.LMode,
        stats.LSTDev, stats.LMin, stats.LMax, stats.LLQ, stats.LUQ};
    r->statistics[1] = (solar_os_imlib_statistics_t){stats.AMean, stats.AMedian, stats.AMode,
        stats.ASTDev, stats.AMin, stats.AMax, stats.ALQ, stats.AUQ};
    r->statistics[2] = (solar_os_imlib_statistics_t){stats.BMean, stats.BMedian, stats.BMode,
        stats.BSTDev, stats.BMin, stats.BMax, stats.BLQ, stats.BUQ};
}
static void imlib_blobs(imlib_work_t *w, image_t *image, list_t *thresholds)
{
    list_t blobs;
    rectangle_t roi = {.w = image->w, .h = image->h};
    solar_os_imlib_options_t *o = &w->options;
    imlib_find_blobs(&blobs, image, &roi, o->x_stride, o->y_stride, thresholds,
        o->invert, o->area_threshold, o->pixels_threshold, o->merge, o->margin,
        imlib_blob_candidate, w, NULL, NULL, 0, 0);
    const float sx = (float)o->image.width / image->w, sy = (float)o->image.height / image->h;
    while (list_size(&blobs)) {
        find_blobs_list_lnk_data_t b; list_pop_front(&blobs, &b);
        if (w->result->count >= o->max_blobs) { w->result->truncated = true; continue; }
        uint32_t x0 = o->image.x + (uint32_t)floorf(b.rect.x * sx);
        uint32_t y0 = o->image.y + (uint32_t)floorf(b.rect.y * sy);
        uint32_t x1 = o->image.x + (uint32_t)ceilf((b.rect.x + b.rect.w) * sx);
        uint32_t y1 = o->image.y + (uint32_t)ceilf((b.rect.y + b.rect.h) * sy);
        w->result->blobs[w->result->count++] = (solar_os_imlib_blob_t){
            .x = x0, .y = y0, .width = x1-x0, .height = y1-y0,
            .cx = o->image.x + b.centroid_x*sx, .cy = o->image.y + b.centroid_y*sy,
            .rotation = atan2f(sinf(b.rotation)*sy, cosf(b.rotation)*sx),
            .pixels = b.pixels, .code = b.code, .count = b.count};
    }
}
static void imlib_execute(imlib_work_t *w)
{
    solar_os_imlib_result_t *r = w->result;
    int64_t start = esp_timer_get_time();
    image_t image = imlib_prepare(w->source, &w->options.image);
    image_t reference = {0};
    if (w->reference) reference = imlib_prepare(w->reference, &w->options.image);
    list_t thresholds; list_init(&thresholds, sizeof(color_thresholds_list_lnk_data_t));
    for (size_t i = 0; i < w->options.threshold_count; ++i) {
        solar_os_imlib_threshold_t *t = &w->options.thresholds[i];
        color_thresholds_list_lnk_data_t v = {t->l_min, t->l_max,
            t->a_min, t->a_max, t->b_min, t->b_max};
        list_push_back(&thresholds, &v);
    }
    r->preprocess_us = esp_timer_get_time() - start;
    solar_os_imlib_poll();
    int64_t process = esp_timer_get_time();
    unsigned k = w->options.ksize;
    switch (r->operation) {
        case SOLAR_OS_IMLIB_HISTOGRAM: case SOLAR_OS_IMLIB_STATISTICS:
            imlib_histogram(w, &image, &thresholds); break;
        case SOLAR_OS_IMLIB_BINARY:
            imlib_binary(&image, &image, &thresholds, w->options.invert, false, NULL); break;
        case SOLAR_OS_IMLIB_INVERT: imlib_invert(&image); break;
        case SOLAR_OS_IMLIB_MEAN: imlib_mean_filter(&image, k, false, 0, false, NULL); break;
        case SOLAR_OS_IMLIB_MEDIAN: imlib_median_filter(&image, k, 0.5f, false, 0, false, NULL); break;
        case SOLAR_OS_IMLIB_GAUSSIAN: {
            int pascal[7], kernel[49]; unsigned n = k*2+1; pascal[0] = 1; int sum = 0;
            for (unsigned i = 0; i < k*2; ++i) pascal[i+1] = pascal[i]*(k*2-i)/(i+1);
            for (unsigned y = 0; y < n; ++y) for (unsigned x = 0; x < n; ++x) {
                kernel[y*n+x] = pascal[y]*pascal[x]; sum += kernel[y*n+x]; }
            imlib_morph(&image, k, kernel, 1.0f/sum, 0, false, 0, false, NULL); break;
        }
        case SOLAR_OS_IMLIB_ERODE: imlib_erode(&image, k, 0, NULL); break;
        case SOLAR_OS_IMLIB_DILATE: imlib_dilate(&image, k, 0, NULL); break;
        case SOLAR_OS_IMLIB_OPENING: imlib_open(&image, k, 0, NULL); break;
        case SOLAR_OS_IMLIB_CLOSING: imlib_close(&image, k, 0, NULL); break;
        case SOLAR_OS_IMLIB_DIFFERENCE: {
            imlib_draw_row_data_t row = {.dst_img = &image};
            for (int y = 0; y < image.h; ++y) { solar_os_imlib_poll();
                row.dst_row_override = reference.data + y*image_line_size(&reference);
                imlib_difference_line_op(0, image.w, y, &row); }
            break;
        }
        case SOLAR_OS_IMLIB_BLOBS: imlib_blobs(w, &image, &thresholds); break;
        default: imlib_fail(ESP_ERR_INVALID_ARG);
    }
    r->process_us = esp_timer_get_time() - process;
    solar_os_imlib_poll();
    int64_t output = esp_timer_get_time();
    if (solar_os_imlib_is_transform(r->operation)) {
        esp_err_t error = solar_os_raster_image_from_pixels(image.data, image_size(&image),
            image.w, image.h, w->options.image.format, 0, &r->image);
        if (error) imlib_fail(error);
    }
    solar_os_imlib_poll();
    r->output_us = esp_timer_get_time() - output;
    r->elapsed_us = esp_timer_get_time() - start;
}
static void imlib_worker(void *user)
{
    imlib_work_t *w = user; imlib_active = w; w->yield_us = esp_timer_get_time();
    if (!setjmp(w->unwind)) imlib_execute(w);
    w->result->workspace_peak_bytes = w->peak;
    while (w->allocations) {
        imlib_allocation_t *n = w->allocations; w->allocations = n->next;
        solar_os_memory_free(n);
    }
    imlib_active = NULL;
    __atomic_store_n(&w->done, true, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}
static esp_err_t imlib_validate(solar_os_raster_image_t *image,
    solar_os_raster_image_t *reference, solar_os_imlib_operation_t op,
    solar_os_imlib_options_t *o)
{
    if (!image || !solar_os_imlib_operation_name(op)) return ESP_ERR_INVALID_ARG;
    if ((op == SOLAR_OS_IMLIB_DIFFERENCE) != (reference != NULL)) return ESP_ERR_INVALID_ARG;
    uint32_t iw = solar_os_raster_image_width(image), ih = solar_os_raster_image_height(image);
    if (reference && (iw != solar_os_raster_image_width(reference) || ih != solar_os_raster_image_height(reference)))
        return ESP_ERR_INVALID_SIZE;
    if (o->image.format != SOLAR_OS_RASTER_IMAGE_GRAY8 &&
        o->image.format != SOLAR_OS_RASTER_IMAGE_RGB565_LE) return ESP_ERR_INVALID_ARG;
    if (o->image.x >= iw || o->image.y >= ih) return ESP_ERR_INVALID_SIZE;
    if (!o->image.width) o->image.width = iw-o->image.x;
    if (!o->image.height) o->image.height = ih-o->image.y;
    if (!o->image.output_width) o->image.output_width = o->image.width;
    if (!o->image.output_height) o->image.output_height = o->image.height;
    if (o->image.width > iw-o->image.x || o->image.height > ih-o->image.y ||
        o->image.output_width > SOLAR_OS_IMLIB_MAX_WIDTH || o->image.output_height > SOLAR_OS_IMLIB_MAX_HEIGHT)
        return ESP_ERR_INVALID_SIZE;
    if (!o->timeout_ms || o->timeout_ms > 60000 || !o->ksize || o->ksize > 3 ||
        (o->bins && (o->bins < 2 || o->bins > SOLAR_OS_IMLIB_BINS_MAX)) ||
        !o->x_stride || !o->y_stride || o->x_stride > o->image.output_width || o->y_stride > o->image.output_height ||
        o->area_threshold > SOLAR_OS_IMLIB_MAX_WIDTH*SOLAR_OS_IMLIB_MAX_HEIGHT ||
        o->pixels_threshold > SOLAR_OS_IMLIB_MAX_WIDTH*SOLAR_OS_IMLIB_MAX_HEIGHT ||
        o->margin > 640 || !o->max_blobs || o->max_blobs > SOLAR_OS_IMLIB_BLOBS_MAX ||
        o->threshold_count > SOLAR_OS_IMLIB_THRESHOLDS_MAX) return ESP_ERR_INVALID_ARG;
    if ((op == SOLAR_OS_IMLIB_BINARY || op == SOLAR_OS_IMLIB_BLOBS) && !o->threshold_count)
        return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < o->threshold_count; ++i) {
        solar_os_imlib_threshold_t *t = &o->thresholds[i];
        if (t->l_min < 0 || t->l_max > (o->image.format == SOLAR_OS_RASTER_IMAGE_GRAY8 ? 255 : 100) ||
            t->l_min > t->l_max || t->a_min < -128 || t->a_max > 127 || t->a_min > t->a_max ||
            t->b_min < -128 || t->b_max > 127 || t->b_min > t->b_max) return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}
esp_err_t solar_os_imlib_run(solar_os_raster_image_t *image,
    solar_os_raster_image_t *reference, solar_os_imlib_operation_t op,
    const solar_os_imlib_options_t *options, solar_os_imlib_cancel_fn cancel,
    void *user, solar_os_imlib_result_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    solar_os_imlib_options_t opt = options ? *options : solar_os_imlib_default_options();
    esp_err_t error = imlib_validate(image, reference, op, &opt);
    if (error) return error;
    if (cancel && cancel(user)) return ESP_ERR_TIMEOUT;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&imlib_busy, &expected, true, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return ESP_ERR_INVALID_STATE;
    imlib_work_t *w = solar_os_memory_calloc(1, sizeof(*w),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "imlib.work");
    error = ESP_ERR_NO_MEM;
    if (!w) goto unlock;
    w->result = solar_os_memory_calloc(1, sizeof(*w->result),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "imlib.result");
    if (!w->result) goto cleanup;
    w->source = image; w->reference = reference; w->options = opt;
    w->result->operation = op; w->result->image_options = opt.image;
    w->deadline_us = esp_timer_get_time() + opt.timeout_ms*1000LL;
    solar_os_raster_image_retain(image); solar_os_raster_image_retain(reference);
    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_external(imlib_worker, "vision-imlib",
        IMLIB_WORKER_STACK, w, tskIDLE_PRIORITY+1, &task, tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND) == pdPASS) {
        bool cancelled = false;
        while (!__atomic_load_n(&w->done, __ATOMIC_ACQUIRE)) {
            if ((cancel && cancel(user)) || esp_timer_get_time() >= w->deadline_us) {
                cancelled = true; __atomic_store_n(&w->cancelled, true, __ATOMIC_RELEASE); }
            vTaskDelay(1);
        }
        solar_os_task_delete_external(task);
        if ((cancel && cancel(user)) || esp_timer_get_time() >= w->deadline_us) cancelled = true;
        error = cancelled ? ESP_ERR_TIMEOUT : w->error;
        if (!error) { *out = w->result; w->result = NULL; }
    }
    solar_os_raster_image_release(reference); solar_os_raster_image_release(image);
cleanup:
    solar_os_imlib_result_free(w->result); solar_os_memory_free(w);
unlock:
    __atomic_store_n(&imlib_busy, false, __ATOMIC_RELEASE);
    return error;
}
