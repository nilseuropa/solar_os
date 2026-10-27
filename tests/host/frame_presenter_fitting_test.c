#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_frame_presenter.h"
#include "solar_os_display.h"

typedef void *TaskHandle_t;
typedef void *SemaphoreHandle_t;
typedef int StaticSemaphore_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define tskNO_AFFINITY 0
#define FRAME_PRESENTER_STACK 3072U
#define FRAME_PRESENTER_RGB565_STACK 6144U
#define FRAME_PRESENTER_PRIORITY 1U
#define FRAME_PRESENTER_HPM_HZ_TENTHS 255U
#define SOLAR_OS_TASK_ROLE_FOREGROUND 0
#define SOLAR_OS_MEMORY_INTERNAL_CRITICAL 0
#define SOLAR_OS_MEMORY_EXTERNAL_PREFERRED 1
#define SOLAR_OS_LOGI(...) ((void)0)
#define SOLAR_OS_LOGW(...) ((void)0)

static size_t display_width, display_height;
static uint32_t display_formats;
static uint16_t display_stack_bytes;
static bool fail_allocation;
static bool rgb565_worker;
static solar_os_display_raster_t captured;

size_t solar_os_gfx_width(const solar_os_gfx_t *gfx)
{ assert(gfx); return display_width; }
size_t solar_os_gfx_height(const solar_os_gfx_t *gfx)
{ assert(gfx); return display_height; }
bool solar_os_gfx_display_target_name(const solar_os_gfx_t *gfx, char *name, size_t size)
{ assert(gfx && size >= sizeof("display0")); strcpy(name, "display0"); return true; }
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{
    assert(!strcmp(name, "display0"));
    *target = (solar_os_display_target_t){
        .frame_formats = display_formats, .preferred_stream_fps = 25,
        .presentation_stack_bytes = display_stack_bytes,
    };
    return true;
}
bool solar_os_gfx_palette_inverted(const solar_os_gfx_t *gfx)
{ assert(gfx); return false; }
esp_err_t solar_os_gfx_present_frame(solar_os_gfx_t *gfx, const solar_os_display_raster_t *frame)
{
    assert(gfx && frame && frame->width && frame->height);
    assert((size_t)frame->x + frame->width <= display_width);
    assert((size_t)frame->y + frame->height <= display_height);
    if (frame->format == SOLAR_OS_DISPLAY_FORMAT_MONO1) {
        /* Match ST7305's requirement: conversion has already scaled the data. */
        assert(frame->source_width == frame->width);
        assert(frame->source_height == frame->height);
        assert(frame->data_size == (size_t)frame->source_stride * frame->height);
    }
    captured = *frame;
    return ESP_OK;
}

static void *solar_os_memory_alloc(size_t size, int policy, const char *owner)
{ (void)policy; (void)owner; assert(size); return fail_allocation ? NULL : malloc(size); }
static void *solar_os_memory_calloc(size_t count, size_t size, int policy, const char *owner)
{ (void)policy; (void)owner; return calloc(count, size); }
static void *solar_os_memory_realloc(void *ptr, size_t size, int policy, const char *owner)
{ (void)policy; (void)owner; assert(size); return realloc(ptr, size); }
static void solar_os_memory_free(void *ptr) { free(ptr); }
static void *xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { return storage; }
static void *xSemaphoreCreateBinaryStatic(StaticSemaphore_t *storage) { return storage; }
static int xSemaphoreTake(void *semaphore, unsigned timeout)
{ assert(semaphore); return timeout ? pdTRUE : 0; }
static void xSemaphoreGive(void *semaphore) { assert(semaphore); }
static void vSemaphoreDelete(void *semaphore) { assert(semaphore); }
static void frame_presenter_worker(void *arg) { (void)arg; assert(false); }
static int solar_os_task_create_pinned_internal(void (*worker)(void *), const char *name,
    unsigned stack, void *arg, unsigned priority, TaskHandle_t *task, int core, int role)
{
    (void)name; (void)priority; (void)core; (void)role;
    assert(worker == frame_presenter_worker && arg);
    unsigned minimum = rgb565_worker ? FRAME_PRESENTER_RGB565_STACK : FRAME_PRESENTER_STACK;
    assert(stack == (display_stack_bytes > minimum ? display_stack_bytes : minimum));
    *task = arg;
    return pdPASS;
}
esp_err_t solar_os_display_set_high_refresh_override(const char *name, bool enabled, uint16_t hz)
{ (void)name; (void)enabled; (void)hz; return ESP_ERR_NOT_SUPPORTED; }

#include "frame_fitting.inc"

/* No host thread runs; suspension only releases the mocked task handle. */
void solar_os_frame_presenter_suspend(solar_os_frame_presenter_t *presenter)
{ presenter->task = NULL; }

static void check_fit(uint16_t width, uint16_t height, uint16_t output_width,
    uint16_t output_height, bool mono, solar_os_frame_fit_t fit)
{
    uint16_t palette[256] = {[1] = 0xffff};
    solar_os_frame_presenter_config_t config = {
        .gfx = (solar_os_gfx_t *)&captured,
        .width = width, .height = height, .stride = width,
        .format = SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565 = palette, .palette_size = 256,
        .fit = fit, .preferred_fps = 35, .allow_mono_fallback = true,
    };
    display_formats = mono ? SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT :
                            SOLAR_OS_DISPLAY_FORMAT_INDEX8_BIT;
    solar_os_frame_presenter_t *p = NULL;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_OK);
    assert(p && p->task);
    assert(p->source_size == (size_t)width * height);
    assert(p->output_width == output_width && p->output_height == output_height);
    assert(p->mono_fallback == mono);
    for (uint16_t y = 0; y < height; ++y)
        for (uint16_t x = 0; x < width; ++x)
            p->presenting[(size_t)y * width + x] = (x < width / 2) != (y < height / 2);
    assert(frame_present(p) == ESP_OK);
    assert(captured.width == output_width && captured.height == output_height);
    if (mono) {
        assert(captured.format == SOLAR_OS_DISPLAY_FORMAT_MONO1);
        /* Black/white quadrants must survive downscaling across the whole frame. */
        for (uint16_t y = 0; y < output_height; ++y)
            for (uint16_t x = 0; x < output_width; ++x) {
                unsigned bit = (p->mono[(size_t)y * p->output_stride + x / 8] >> (x % 8)) & 1;
                assert(bit == ((x < output_width / 2) == (y < output_height / 2)));
            }
    } else {
        assert(captured.source_width == width && captured.source_height == height);
        assert(captured.data == p->presenting);
    }
    solar_os_frame_presenter_suspend(p);
    assert(solar_os_frame_presenter_resume(p) == ESP_OK);
    assert(p->output_width == output_width && p->output_height == output_height);
    assert(solar_os_frame_presenter_deinit(p) == ESP_OK);
}

int main(void)
{
    display_width = 384; display_height = 168;
    check_fit(320, 240, 224, 168, true, SOLAR_OS_FRAME_FIT_HEIGHT); /* CL32 DOOM */
    check_fit(640, 480, 224, 168, true, SOLAR_OS_FRAME_FIT_HEIGHT); /* Both axes larger */
    display_width = 168; display_height = 384;
    check_fit(320, 240, 168, 126, true, SOLAR_OS_FRAME_FIT_HEIGHT); /* Width limited */
    display_width = 320; display_height = 240;
    check_fit(640, 480, 320, 240, false, SOLAR_OS_FRAME_FIT_HEIGHT); /* Direct raster */
    check_fit(320, 240, 320, 240, false, SOLAR_OS_FRAME_FIT_DEFAULT);
    display_stack_bytes = 8192; /* DLP needs deeper protocol and socket calls. */
    check_fit(320, 240, 320, 240, false, SOLAR_OS_FRAME_FIT_DEFAULT);
    display_stack_bytes = 1024; /* A target cannot reduce the worker's default. */
    check_fit(320, 240, 320, 240, false, SOLAR_OS_FRAME_FIT_DEFAULT);
    display_stack_bytes = 0;
    display_width = 400; display_height = 300;
    check_fit(320, 240, 400, 300, true, SOLAR_OS_FRAME_FIT_HEIGHT); /* SolarTerm */

    uint16_t palette[256] = {0};
    solar_os_frame_presenter_config_t config = {
        .gfx = (solar_os_gfx_t *)&captured, .width = 320, .height = 240, .stride = 320,
        .format = SOLAR_OS_DISPLAY_FORMAT_INDEX8,
        .palette_rgb565 = palette, .palette_size = 256,
        .fit = SOLAR_OS_FRAME_FIT_HEIGHT, .allow_mono_fallback = true,
    };
    solar_os_frame_presenter_t *p = NULL;
    config.width = 0;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    config.width = 320; config.height = 0;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    config.height = 240; config.stride = 319;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    config.stride = 320; display_width = 0;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    display_width = 384; display_height = 0;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    display_height = 168;
    config.fit = SOLAR_OS_FRAME_FIT_DEFAULT;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_NOT_SUPPORTED && !p);
    config.fit = SOLAR_OS_FRAME_FIT_HEIGHT; config.width = 1; config.height = UINT16_MAX;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_NOT_SUPPORTED && !p);

    config.width = 320; config.height = 240;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_OK);
    uint8_t *writing = (void *)1;
    size_t bytes = 1;
    fail_allocation = true;
    assert(solar_os_frame_presenter_acquire(p, &writing, &bytes) == ESP_ERR_NO_MEM);
    assert(!writing && !bytes);
    fail_allocation = false;
    uint8_t *legacy = malloc(p->source_size);
    memset(legacy, 42, p->source_size);
    assert(solar_os_frame_presenter_submit(p, legacy, p->source_size));
    legacy[0] = 9;
    assert(p->queued[0] == 42); /* Copying API keeps caller ownership. */
    assert(solar_os_frame_presenter_acquire(p, &writing, &bytes) == ESP_OK);
    assert(bytes == p->source_size && writing != p->presenting && writing != p->queued);
    assert(!solar_os_frame_presenter_submit(p, legacy, p->source_size));
    uint8_t *duplicate = NULL;
    size_t duplicate_size = 0;
    assert(solar_os_frame_presenter_acquire(p, &duplicate, &duplicate_size) == ESP_ERR_INVALID_STATE);
    assert(solar_os_frame_presenter_exchange(p, &legacy) == ESP_ERR_INVALID_STATE);
    free(legacy);
    const uint8_t *completed = NULL;
    unsigned previous_value = 0;
    for (unsigned i = 1; i <= 300; ++i) {
        memset(writing, i, bytes);
        if (completed) {
            assert(completed != writing);
            for (size_t j = 0; j < bytes; ++j) assert(completed[j] == (uint8_t)previous_value);
        }
        completed = writing;
        bool replaced = p->queued_ready;
        unsigned replacements = p->stats.replaced_frames;
        assert(solar_os_frame_presenter_exchange(p, &writing) == ESP_OK);
        assert(p->queued == completed && p->queued_ready);
        assert(writing != p->queued && writing != p->presenting);
        assert(p->stats.replaced_frames == replacements + replaced);
        previous_value = i;
        if (i % 3) {
            /* Display worker consumes the queued frame between submissions. */
            uint8_t *old = p->presenting;
            p->presenting = p->queued; p->queued = old;
            p->queued_ready = false; p->presenting_valid = true;
            assert(frame_present(p) == ESP_OK);
        }
        if (!(i % 5)) assert(solar_os_frame_presenter_redraw(p));
        if (!(i % 7)) {
            solar_os_frame_presenter_suspend(p);
            assert(solar_os_frame_presenter_exchange(p, &writing) == ESP_ERR_INVALID_STATE);
            assert(solar_os_frame_presenter_resume(p) == ESP_OK);
            assert(p->writing == writing);
        }
    }
    p->stop_requested = true;
    assert(solar_os_frame_presenter_exchange(p, &writing) == ESP_ERR_INVALID_STATE);
    p->stop_requested = false;
    solar_os_frame_presenter_suspend(p);
    display_width = 168; display_height = 384;
    assert(solar_os_frame_presenter_resume(p) == ESP_OK);
    assert(p->output_width == 168 && p->output_height == 126);
    assert(frame_present(p) == ESP_OK);
    solar_os_frame_presenter_suspend(p);
    display_width = 0;
    assert(solar_os_frame_presenter_resume(p) == ESP_ERR_NOT_SUPPORTED);
    assert(!p->task);
    assert(solar_os_frame_presenter_deinit(p) == ESP_OK);

    /* RGB565 needs two bytes per pixel and no palette. Mono conversion is
     * opt-in; a target supporting both formats always takes the direct path. */
    display_width = 64; display_height = 48;
    display_formats = SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    config = (solar_os_frame_presenter_config_t){
        .gfx = (solar_os_gfx_t *)&captured, .format = SOLAR_OS_DISPLAY_FORMAT_RGB565,
        .width = 64, .height = 48, .stride = 128,
        .fit = SOLAR_OS_FRAME_FIT_HEIGHT,
    };
    p = NULL;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_NOT_SUPPORTED && !p);
    rgb565_worker = true;
    config.allow_mono_fallback = true;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_OK && p->mono_fallback);
    assert(solar_os_frame_presenter_acquire(p, &writing, &bytes) == ESP_OK);
    memset(writing, 0xff, bytes);
    assert(solar_os_frame_presenter_exchange(p, &writing) == ESP_OK);
    uint8_t *swap = p->presenting; p->presenting = p->queued; p->queued = swap;
    assert(frame_present(p) == ESP_OK && captured.format == SOLAR_OS_DISPLAY_FORMAT_MONO1);
    assert(captured.source_stride == 8 && captured.data_size == 8 * 48);
    for (unsigned i = 0; i < captured.data_size; ++i) assert(captured.data[i] == 0);
    solar_os_frame_presenter_suspend(p);
    display_width = 37; display_height = 35;
    assert(solar_os_frame_presenter_resume(p) == ESP_OK);
    assert(p->output_width == 37 && p->output_height == 28 && p->output_x % 8 == 0);
    assert(frame_present(p) == ESP_OK && captured.source_stride == 5);
    assert(solar_os_frame_presenter_deinit(p) == ESP_OK);
    display_width = 64; display_height = 48;
    display_formats = SOLAR_OS_DISPLAY_FORMAT_RGB565_BIT | SOLAR_OS_DISPLAY_FORMAT_MONO1_BIT;
    p = NULL;
    config.stride = 127;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_ERR_INVALID_ARG && !p);
    config.stride = 128;
    assert(solar_os_frame_presenter_init(&p, &config) == ESP_OK);
    assert(!p->mono_fallback && !p->mono);
    assert(solar_os_frame_presenter_acquire(p, &writing, &bytes) == ESP_OK);
    assert(bytes == 64 * 48 * 2);
    writing[0] = 0xf8; writing[1] = 0;
    const uint8_t *rgb_frame = writing;
    assert(solar_os_frame_presenter_exchange(p, &writing) == ESP_OK);
    assert(p->queued == rgb_frame && writing != rgb_frame);
    uint8_t *old_active = p->presenting;
    p->presenting = p->queued; p->queued = old_active;
    assert(frame_present(p) == ESP_OK && captured.data == rgb_frame);
    assert(!captured.palette_inverted && captured.palette_rgb565 == NULL);
    assert(solar_os_frame_presenter_deinit(p) == ESP_OK);

    puts("Presenter fits oversized source frames on mono and direct displays");
    return 0;
}
