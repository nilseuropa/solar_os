#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "solar_os_vision.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"
#include "solar_os_gfx.h"

static atomic_int allocations, failures = -1, workers;
static atomic_llong timer_offset;
static bool deny_worker;
static int attempts;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t kind, const char *tag)
{
    (void)tag; (void)kind;
    if (atomic_load(&failures) >= 0 && atomic_fetch_sub(&failures, 1) == 0) return NULL;
    void *p = malloc(size);
    if (p) atomic_fetch_add(&allocations, 1);
    return p;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t kind, const char *tag)
{
    void *p = solar_os_memory_alloc(n * size, kind, tag);
    if (p) memset(p, 0, n * size);
    return p;
}
void solar_os_memory_free(void *p)
{
    if (p) { assert(atomic_fetch_sub(&allocations, 1) > 0); free(p); }
}
void *solar_os_quirc_malloc(size_t size)
{ return solar_os_memory_alloc(size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "quirc"); }
void *solar_os_quirc_calloc(size_t n, size_t size)
{ return solar_os_memory_calloc(n, size, SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "quirc"); }
void solar_os_quirc_free(void *p) { solar_os_memory_free(p); }

/* Real stb JPEG/PNG decode, with tracked native allocations. */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(n) solar_os_quirc_malloc(n)
#define STBI_FREE(p) solar_os_memory_free(p)
static void *test_realloc(void *p, size_t old, size_t size)
{
    void *next = solar_os_quirc_malloc(size);
    if (next) { if (p) memcpy(next, p, old < size ? old : size); solar_os_memory_free(p); }
    return next;
}
#define STBI_REALLOC_SIZED(p, old, n) test_realloc(p, old, n)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

esp_err_t solar_os_stb_decode_rgb(const uint8_t *data, size_t length, uint32_t max,
    uint8_t **pixels, uint32_t *width, uint32_t *height)
{
    int w, h, channels;
    uint8_t *p = stbi_load_from_memory(data, length, &w, &h, &channels, 3);
    if (!p) return ESP_ERR_INVALID_RESPONSE;
    if ((uint64_t)w * h > max) { stbi_image_free(p); return ESP_ERR_INVALID_SIZE; }
    *pixels = p; *width = w; *height = h; return ESP_OK;
}
esp_err_t solar_os_webp_decode_rgb(const uint8_t *data, size_t length, uint32_t max,
    uint8_t **pixels, uint32_t *width, uint32_t *height)
{ (void)data; (void)length; (void)max; (void)pixels; (void)width; (void)height; return ESP_ERR_NOT_SUPPORTED; }
void solar_os_stb_image_free(void *p) { stbi_image_free(p); }
void solar_os_webp_free(void *p) { solar_os_memory_free(p); }
size_t solar_os_gfx_width(const solar_os_gfx_t *gfx) { (void)gfx; return 320; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx) { (void)gfx; return 240; }
bool solar_os_gfx_supports_frame_format(const solar_os_gfx_t *gfx, solar_os_display_format_t format)
{ (void)gfx; (void)format; return false; }
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *raster)
{ (void)gfx; (void)raster; return ESP_OK; }
esp_err_t solar_os_gfx_blit_raster(solar_os_gfx_t *gfx, const solar_os_gfx_raster_t *raster,
    int x, int y, int w, int h, const solar_os_gfx_clip_t *clip)
{ (void)gfx; (void)raster; (void)x; (void)y; (void)w; (void)h; (void)clip; return ESP_OK; }
void solar_os_gfx_present(solar_os_gfx_t *gfx) { (void)gfx; }

struct test_task { pthread_t thread; TaskFunction_t run; void *user; };
static void *test_worker(void *user)
{
    struct test_task *task = user;
    task->run(task->user);
    return NULL;
}
BaseType_t solar_os_task_create_pinned_external(TaskFunction_t run, const char *name,
    uint32_t stack, void *user, UBaseType_t priority, TaskHandle_t *handle,
    BaseType_t core, solar_os_task_role_t role)
{
    (void)name; (void)priority; (void)core;
    assert(stack >= (!strcmp(name, "vision-imlib") ? 12U : 20U) * 1024U &&
        role == SOLAR_OS_TASK_ROLE_FOREGROUND);
    ++attempts;
    if (deny_worker) return 0;
    struct test_task *task = malloc(sizeof(*task)); assert(task);
    task->run = run; task->user = user; *handle = task;
    atomic_fetch_add(&workers, 1);
    assert(!pthread_create(&task->thread, NULL, test_worker, task));
    return pdPASS;
}
void solar_os_task_delete_external(TaskHandle_t task)
{
    assert(!pthread_join(task->thread, NULL)); free(task);
    atomic_fetch_sub(&workers, 1);
}
int64_t esp_timer_get_time(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000 + atomic_load(&timer_offset);
}
void vTaskDelay(TickType_t ticks)
{
    struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = (ticks % 1000) * 1000000};
    nanosleep(&t, NULL);
}
void vTaskSuspend(TaskHandle_t task) { assert(!task); pthread_exit(NULL); }
bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, uint32_t ms)
{
    (void)task; (void)ms; vTaskDelay(1);
    return __atomic_load_n(done, __ATOMIC_ACQUIRE);
}

static const uint8_t payload[] = "SolarOS QR\0binary\xff";
static void check_payload(solar_os_vision_qr_results_t *result, size_t count)
{
    assert(result && result->count == count);
    for (size_t i = 0; i < count; ++i) {
        assert(result->codes[i].length == sizeof(payload) - 1);
        assert(!memcmp(result->codes[i].payload, payload, sizeof(payload) - 1));
        assert(result->codes[i].version == 2);
    }
}
static solar_os_raster_image_t *open_fixture(const char *name)
{
    char path[256]; snprintf(path, sizeof(path), "../fixtures/vision/%s", name);
    solar_os_raster_image_t *image = NULL;
    assert(solar_os_raster_image_open(path, &image) == ESP_OK);
    return image;
}
static bool cancel_immediately(void *user) { (void)user; return true; }
static int cancel_polls;
static bool cancel_running(void *user) { (void)user; return ++cancel_polls > 2; }
static int timeout_polls;
static bool expire_deadline(void *user)
{
    (void)user;
    if (++timeout_polls == 2) atomic_store(&timer_offset, 6000000);
    return false;
}
static int busy_polls;
static bool compete(void *user)
{
    if (++busy_polls == 2) {
        solar_os_vision_qr_results_t *result = (void *)1;
        assert(solar_os_vision_qrcodes(user, NULL, NULL, NULL, &result) == ESP_ERR_INVALID_STATE);
        assert(!result);
    }
    return false;
}

int main(void)
{
    for (int i = 0; i < 4; ++i) {
        const char *names[] = {"qr.png", "qr.jpg", "mirrored.png", "multiple.png"};
        solar_os_raster_image_t *image = open_fixture(names[i]);
        solar_os_vision_qr_results_t *result = NULL;
        assert(solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result) == ESP_OK);
        solar_os_raster_image_release(image); /* Results own their payloads. */
        check_payload(result, i == 3 ? 2 : 1);
        solar_os_vision_qr_results_free(result);
        assert(!allocations && !workers);
    }
    solar_os_raster_image_t *image = open_fixture("multiple.png");
    int baseline = allocations;
    solar_os_vision_qr_results_t *result = NULL;
    const solar_os_raster_image_convert_options_t crop = {
        .x = 218, .y = 12, .width = 198, .height = 180, .output_width = 99, .output_height = 90,
    };
    assert(solar_os_vision_qrcodes(image, &crop, NULL, NULL, &result) == ESP_OK);
    check_payload(result, 1);
    int32_t min_x = INT32_MAX, min_y = INT32_MAX;
    for (int j = 0; j < 4; ++j) {
        if (result->codes[0].corners[j].x < min_x) min_x = result->codes[0].corners[j].x;
        if (result->codes[0].corners[j].y < min_y) min_y = result->codes[0].corners[j].y;
    }
    assert(min_x >= 240 && min_x <= 244 && min_y >= 22 && min_y <= 26);
    assert(result->processed_width == 99 && result->width == 416);
    solar_os_vision_qr_results_free(result);
    assert(allocations == baseline);
    int calls = attempts;
    assert(solar_os_vision_qrcodes(image, NULL, cancel_immediately, NULL, &result) == ESP_ERR_TIMEOUT);
    assert(!result && calls == attempts && allocations == baseline);
    assert(solar_os_vision_qrcodes(image, NULL, cancel_running, NULL, &result) == ESP_ERR_TIMEOUT);
    assert(!result && allocations == baseline && !workers);
    assert(solar_os_vision_qrcodes(image, NULL, expire_deadline, NULL, &result) == ESP_ERR_TIMEOUT);
    atomic_store(&timer_offset, 0);
    assert(!result && allocations == baseline && !workers);
    assert(solar_os_vision_qrcodes(image, NULL, compete, image, &result) == ESP_OK);
    assert(busy_polls >= 2); solar_os_vision_qr_results_free(result);
    deny_worker = true;
    assert(solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result) == ESP_ERR_NO_MEM);
    assert(!result && allocations == baseline); deny_worker = false;
    bool success = false;
    for (int n = 0; n < 15; ++n) {
        atomic_store(&failures, n);
        esp_err_t err = solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result);
        atomic_store(&failures, -1);
        if (err == ESP_OK) { success = true; check_payload(result, 2); solar_os_vision_qr_results_free(result); }
        else assert(err == ESP_ERR_NO_MEM && !result);
        assert(allocations == baseline && !workers);
    }
    assert(success);
    solar_os_raster_image_convert_options_t invalid = {.x = UINT32_MAX};
    assert(solar_os_vision_qrcodes(image, &invalid, NULL, NULL, &result) == ESP_ERR_INVALID_SIZE && !result);
    invalid = (solar_os_raster_image_convert_options_t){.output_width = 641};
    assert(solar_os_vision_qrcodes(image, &invalid, NULL, NULL, &result) == ESP_ERR_INVALID_SIZE && !result);
    solar_os_raster_image_release(image); assert(!allocations);
    image = open_fixture("blank.png");
    assert(solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result) == ESP_OK && result->count == 0);
    solar_os_vision_qr_results_free(result); solar_os_raster_image_release(image);
    image = open_fixture("corrupt.png");
    assert(solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result) == ESP_OK);
    assert(result->count == 0 && result->decode_failures > 0);
    solar_os_vision_qr_results_free(result); solar_os_raster_image_release(image);
    image = open_fixture("many.png");
    assert(solar_os_vision_qrcodes(image, NULL, NULL, NULL, &result) == ESP_OK);
    check_payload(result, 8); assert(result->truncated && result->candidates > 8);
    solar_os_vision_qr_results_free(result); solar_os_raster_image_release(image);
    assert(!allocations && !workers);
    puts("native QR JPEG/PNG/binary/mirror/crop/cancel/busy/OOM tests passed");
    return 0;
}
