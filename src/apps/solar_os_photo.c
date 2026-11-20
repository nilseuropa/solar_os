#include "solar_os_photo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_camera.h"
#include "solar_os_config.h"
#include "solar_os_display.h"
#include "solar_os_gfx.h"
#include "solar_os_keys.h"
#include "solar_os_media_widgets.h"
#include "solar_os_memory.h"
#include "solar_os_script_media.h"
#include "solar_os_stb_image.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define PHOTO_STACK 24576U
#define PHOTO_SAVE_STACK 8192U
#define PHOTO_MAX_PIXELS (2U * 1024U * 1024U)
#define PHOTO_SOURCE_MAX 256U
#if !CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PHOTO_STACK);
#endif
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PHOTO_SAVE_STACK);

typedef struct {
    unsigned refs;
    uint8_t *jpeg, *pixels;
    size_t jpeg_size;
    uint32_t width, height, generation;
} photo_frame_t;

typedef struct {
    SemaphoreHandle_t mutex;
    TaskHandle_t worker, save_worker;
    volatile bool done, save_done;
    bool stop, suspended;
    bool remote, monochrome, direct, fullscreen, high_refresh;
    bool dirty, layout_dirty, capture_busy;
    uint8_t key;
    int pointer_x, pointer_y;
    uint16_t requested_width, requested_height;
    uint32_t output_width, output_height, generation, sequence;
    photo_frame_t *pending, *current, *capture;
    esp_err_t error;
    char message[96], source[PHOTO_SOURCE_MAX], label[PHOTO_SOURCE_MAX];
    char directory[SOLAR_OS_STORAGE_PATH_MAX];
    char display_target[SOLAR_OS_DISPLAY_TARGET_NAME_MAX];
} photo_state_t;

static void *photo_state;
#define photo (*(photo_state_t *)photo_state)

static bool stopping(void)
{ return __atomic_load_n(&photo.stop, __ATOMIC_RELAXED); }
static bool suspended(void)
{ return __atomic_load_n(&photo.suspended, __ATOMIC_RELAXED); }
static bool cancelled(void *user)
{ (void)user; return stopping() || suspended(); }

static void frame_release(photo_frame_t *frame)
{
    if (!frame || __atomic_sub_fetch(&frame->refs, 1U, __ATOMIC_ACQ_REL)) return;
    solar_os_memory_free(frame->jpeg);
    solar_os_stb_image_free(frame->pixels);
    solar_os_memory_free(frame);
}

/* Only the worker writes storage. Exclusive creation never replaces a photo;
 * a failed or cancelled write removes only the new file owned by this call. */
static esp_err_t save_frame(const photo_frame_t *frame, char *name, size_t size)
{
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    int fd = -1;
    while (photo.sequence < 1000000U && !stopping()) {
        snprintf(name, size, "photo-%06lu.jpg", (unsigned long)photo.sequence++);
        const int length = snprintf(path, sizeof(path), "%s%s%s", photo.directory,
            photo.directory[strlen(photo.directory) - 1] == '/' ? "" : "/", name);
        if (length < 0 || (size_t)length >= sizeof(path)) { errno = ENAMETOOLONG; return ESP_FAIL; }
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0) break;
        if (errno != EEXIST) return ESP_FAIL;
    }
    if (fd < 0) { errno = stopping() ? ECANCELED : ENOSPC; return ESP_FAIL; }
    size_t written = 0;
    int failure = 0;
    while (written < frame->jpeg_size && !stopping()) {
        size_t bytes = frame->jpeg_size - written;
        if (bytes > 4096U) bytes = 4096U;
        const ssize_t result = write(fd, frame->jpeg + written, bytes);
        if (result <= 0) { failure = errno ? errno : EIO; break; }
        written += (size_t)result;
    }
    if (!failure && written != frame->jpeg_size) failure = ECANCELED;
    if (close(fd) != 0 && !failure) failure = errno ? errno : EIO;
    if (failure) { (void)unlink(path); errno = failure; return ESP_FAIL; }
    solar_os_storage_notify_changed();
    return ESP_OK;
}

static void save_requested(void)
{
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    photo_frame_t *frame = photo.capture;
    photo.capture = NULL;
    xSemaphoreGive(photo.mutex);
    if (!frame) return;
    char name[32];
    const esp_err_t error = save_frame(frame, name, sizeof(name));
    const int failure = errno;
    frame_release(frame);
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    if (error == ESP_OK) snprintf(photo.message, sizeof(photo.message), "Saved %s", name);
    else snprintf(photo.message, sizeof(photo.message), "Save failed: %s", strerror(failure));
    photo.capture_busy = false;
    photo.dirty = true;
    xSemaphoreGive(photo.mutex);
}

static void save_worker(void *user)
{
    (void)user;
    save_requested();
    __atomic_store_n(&photo.save_done, true, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}

static esp_err_t prepare_frame(const solar_os_script_media_frame_t *source,
    photo_frame_t **out)
{
    *out = NULL;
    photo_frame_t *frame = solar_os_memory_calloc(1, sizeof(*frame),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "photo.frame");
    if (!frame) return ESP_ERR_NO_MEM;
    frame->refs = 1;
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    const uint32_t width = photo.output_width, height = photo.output_height;
    frame->generation = photo.generation;
    xSemaphoreGive(photo.mutex);
    esp_err_t error;
    if (photo.monochrome)
        error = solar_os_stb_jpeg_decode_gray(source->jpeg.data, source->jpeg.length,
            PHOTO_MAX_PIXELS, &frame->pixels, &frame->width, &frame->height);
    else if (photo.direct)
        error = solar_os_stb_decode_jpeg_rgb565_scaled(source->jpeg.data, source->jpeg.length,
            PHOTO_MAX_PIXELS, width, height, &frame->pixels, &frame->width, &frame->height);
    else
        error = solar_os_stb_decode_jpeg_rgb_scaled(source->jpeg.data, source->jpeg.length,
            PHOTO_MAX_PIXELS, width, height, &frame->pixels, &frame->width, &frame->height);
    if (error == ESP_OK) {
        frame->jpeg = solar_os_memory_alloc(source->jpeg.length,
            SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "photo.jpeg");
        if (!frame->jpeg) error = ESP_ERR_NO_MEM;
        else { memcpy(frame->jpeg, source->jpeg.data, source->jpeg.length); frame->jpeg_size = source->jpeg.length; }
    }
    if (error != ESP_OK) { frame_release(frame); return error; }
    *out = frame;
    return ESP_OK;
}

static esp_err_t open_source(solar_os_script_media_t *media, uint32_t *handle)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (photo.remote) return solar_os_script_media_rtsp_open(media, photo.source, true, false, handle);
#endif
    solar_os_stream_info_t info;
    esp_err_t error = solar_os_stream_get_info(photo.source, &info);
    if (error != ESP_OK) return error;
    if (info.type != SOLAR_OS_STREAM_TYPE_VIDEO || info.direction == SOLAR_OS_STREAM_DIRECTION_SINK ||
        info.video.codec != SOLAR_OS_STREAM_VIDEO_JPEG) return ESP_ERR_NOT_SUPPORTED;
    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = photo.requested_width, .height = photo.requested_height},
    };
    return solar_os_script_media_open(media, photo.source, &options, handle);
}

static void worker(void *user)
{
    (void)user;
    solar_os_script_media_t *media = NULL;
    uint32_t handle = 0;
    esp_err_t error = ESP_OK;
    unsigned failures = 0;
    int64_t last_frame = 0;
    while (!stopping()) {
        if (suspended()) {
            solar_os_script_media_destroy(media); media = NULL; handle = 0;
            vTaskDelay(pdMS_TO_TICKS(20) + 1); continue;
        }
        if (!media) {
            error = solar_os_script_media_create("app:photo", cancelled, NULL, &media);
            if (error == ESP_OK) error = open_source(media, &handle);
            if (error != ESP_OK) {
                if (!cancelled(NULL)) break;
                solar_os_script_media_destroy(media); media = NULL; handle = 0;
                error = ESP_OK; continue;
            }
        }
        xSemaphoreTake(photo.mutex, portMAX_DELAY);
        const bool pending = photo.pending != NULL;
        xSemaphoreGive(photo.mutex);
        if (pending || esp_timer_get_time() - last_frame < 100000) {
            vTaskDelay(pdMS_TO_TICKS(10) + 1); continue;
        }
        uint32_t lease = 0;
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
        if (photo.remote) {
            error = solar_os_script_media_rtsp_read(media, handle, 50, &lease);
            if (error == ESP_OK && !lease) {
                solar_os_rtsp_client_status_t status; bool ended;
                error = solar_os_script_media_rtsp_status(media, handle, &status, &ended);
                if (error == ESP_OK && (ended || (status.negotiated && !status.video)))
                    error = status.error != ESP_OK ? status.error : ESP_ERR_NOT_SUPPORTED;
                if (error == ESP_OK) continue;
            }
        } else
#endif
            error = solar_os_script_media_acquire(media, handle, &lease);
        photo_frame_t *frame = NULL;
        if (error == ESP_OK) {
            solar_os_script_media_frame_t source;
            error = solar_os_script_media_frame(media, lease, &source);
            if (error == ESP_OK) error = prepare_frame(&source, &frame);
        }
        if (lease) {
            const esp_err_t release = solar_os_script_media_release(media, lease);
            if (error == ESP_OK) error = release;
        }
        if (error != ESP_OK) {
            frame_release(frame);
            if (cancelled(NULL)) { error = ESP_OK; continue; }
            if (++failures >= 3 || (error != ESP_ERR_TIMEOUT && error != ESP_ERR_INVALID_RESPONSE)) break;
            continue;
        }
        failures = 0;
        last_frame = esp_timer_get_time();
        xSemaphoreTake(photo.mutex, portMAX_DELAY);
        if (cancelled(NULL) || frame->generation != photo.generation) frame_release(frame);
        else { photo.pending = frame; }
        xSemaphoreGive(photo.mutex);
    }
    solar_os_script_media_destroy(media);
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    if (error != ESP_OK) photo.error = error;
    xSemaphoreGive(photo.mutex);
    __atomic_store_n(&photo.done, true, __ATOMIC_RELEASE);
    for (;;) vTaskSuspend(NULL);
}

static void refresh(solar_os_context_t *ctx, bool enabled)
{
    if (photo.high_refresh == enabled) return;
    if (enabled && !solar_os_gfx_display_target_name(solar_os_context_gfx(ctx),
        photo.display_target, sizeof(photo.display_target))) return;
    if (solar_os_display_set_high_refresh_override(photo.display_target, enabled, 255U) == ESP_OK)
        photo.high_refresh = enabled;
}

static solar_os_display_rect_t viewport(solar_os_gfx_t *gfx)
{
    if (photo.fullscreen) return solar_os_gfx_panel_bounds(gfx);
    solar_os_media_player_layout_t ui;
    solar_os_media_player_layout(solar_os_gfx_width(gfx), solar_os_gfx_height(gfx), false, &ui);
    return (solar_os_display_rect_t){ui.view_x, ui.view_y, ui.view_width, ui.view_height};
}

static void layout(solar_os_context_t *ctx)
{
    const solar_os_display_rect_t rect = viewport(solar_os_context_gfx(ctx));
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    photo.output_width = rect.width; photo.output_height = rect.height;
    photo.generation++;
    frame_release(photo.pending); photo.pending = NULL;
    photo.dirty = photo.layout_dirty = true;
    xSemaphoreGive(photo.mutex);
}

static void draw_controls(solar_os_gfx_t *gfx)
{
    const int width = solar_os_gfx_width(gfx), height = solar_os_gfx_height(gfx);
    solar_os_media_player_layout_t ui;
    solar_os_media_player_layout(width, height, false, &ui);
    solar_os_media_player_header_draw(gfx, width, "Photo", "F: full");
    solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
    solar_os_gfx_line(gfx, 0, ui.controls_y, width - 1, ui.controls_y);
    if (ui.title_y >= 0) {
        solar_os_gfx_set_font(gfx, ui.compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_BOLD_16);
        solar_os_media_text_draw(gfx, 3, ui.title_y, width - 6, photo.label);
    }
    solar_os_media_player_status_draw(gfx, width, height, photo.message[0] ? photo.message :
        photo.current ? "Ready" : "Waiting for video...");
    if (ui.volume_y >= 0) {
        const char literal_key[2] = {(char)photo.key, '\0'};
        const char *key_name = solar_os_key_name(photo.key);
        char hint[64];
        snprintf(hint, sizeof(hint), "%s: capture", key_name ? key_name : literal_key);
        solar_os_gfx_set_font(gfx, ui.compact ? SOLAR_OS_GFX_FONT_BITMAP_8 : SOLAR_OS_GFX_FONT_SMALL);
        solar_os_media_text_draw(gfx, 3, ui.volume_y + (ui.compact ? 5 : 9), width - 6, hint);
    }
    solar_os_media_transport_button_draw(gfx, ui.button_gap * 2 + ui.button_width,
        ui.button_y, ui.button_width, ui.button_height,
        SOLAR_OS_MEDIA_TRANSPORT_RECORD, photo.capture_busy);
}

static void render(solar_os_context_t *ctx)
{
    if (suspended() || xSemaphoreTake(photo.mutex, 0) != pdTRUE) return;
    bool changed = false;
    if (photo.pending) {
        if (!photo.current) photo.dirty = true;
        frame_release(photo.current); photo.current = photo.pending; photo.pending = NULL;
        changed = true;
    }
    if (!changed && !photo.dirty) { xSemaphoreGive(photo.mutex); return; }
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    const solar_os_display_rect_t rect = viewport(gfx);
    const bool chrome = photo.dirty || photo.layout_dirty;
    if (chrome) {
        solar_os_gfx_clear(gfx, SOLAR_OS_GFX_COLOR_WHITE);
        solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_SMALL);
        solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
        if (!photo.fullscreen) draw_controls(gfx);
        if (!photo.current) {
            solar_os_gfx_set_color(gfx, SOLAR_OS_GFX_COLOR_BLACK);
            solar_os_gfx_set_font(gfx, SOLAR_OS_GFX_FONT_BITMAP_8);
            solar_os_media_text_draw(gfx, rect.x + 2, rect.y + 8, rect.width - 4, "Waiting for video...");
        }
    }
    photo_frame_t *frame = photo.current;
    if (frame) {
        uint32_t w = rect.width, h = (uint64_t)frame->height * w / frame->width;
        if (h > (uint32_t)rect.height) { h = rect.height; w = (uint64_t)frame->width * h / frame->height; }
        if (!w) w = 1;
        if (!h) h = 1;
        const int x = rect.x + (rect.width - (int)w) / 2, y = rect.y + (rect.height - (int)h) / 2;
        if (photo.direct) {
            if (chrome) solar_os_gfx_present(gfx);
            const solar_os_display_raster_t raster = {.data = frame->pixels,
                .data_size = (size_t)frame->width * frame->height * 2U,
                .source_width = frame->width, .source_height = frame->height,
                .source_stride = frame->width * 2U, .width = w, .height = h,
                .format = SOLAR_OS_DISPLAY_FORMAT_RGB565};
            const esp_err_t error = solar_os_gfx_present_frame_at(gfx, &raster, x, y);
            if (error != ESP_OK) photo.error = error;
        } else {
            const unsigned channels = photo.monochrome ? 1U : 3U;
            const solar_os_gfx_raster_t raster = {.pixels = frame->pixels,
                .pixels_size = (size_t)frame->width * frame->height * channels,
                .width = frame->width, .height = frame->height, .stride = frame->width * channels,
                .format = photo.monochrome ? SOLAR_OS_GFX_RASTER_GRAY8 : SOLAR_OS_GFX_RASTER_RGB888};
            const esp_err_t error = solar_os_gfx_blit_raster(gfx, &raster, x, y, w, h, NULL);
            if (error != ESP_OK) photo.error = error;
            solar_os_gfx_present(gfx);
        }
    } else if (chrome) solar_os_gfx_present(gfx);
    photo.dirty = photo.layout_dirty = false;
    xSemaphoreGive(photo.mutex);
}

static esp_err_t parse(solar_os_context_t *ctx)
{
    const char *source = "camera0", *directory = ".";
    bool have_source = false, have_dir = false, have_key = false, have_size = false;
    photo.key = SOLAR_OS_KEY_ENTER; photo.sequence = 1;
    for (int i = 1; i < solar_os_context_argc(ctx); i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (!strcmp(arg, "--fullscreen") && !photo.fullscreen) {
            photo.fullscreen = true;
        } else if (!strcmp(arg, "--dir") && !have_dir) {
            if (++i == solar_os_context_argc(ctx)) return ESP_ERR_INVALID_ARG;
            directory = solar_os_context_argv(ctx, i); have_dir = true;
        } else if (!strcmp(arg, "--key") && !have_key) {
            if (++i == solar_os_context_argc(ctx) ||
                !solar_os_key_parse(solar_os_context_argv(ctx, i), &photo.key) ||
                photo.key == SOLAR_OS_KEY_ESCAPE || photo.key == SOLAR_OS_KEY_APP_EXIT ||
                photo.key == 'f' || photo.key == 'F') return ESP_ERR_INVALID_ARG;
            have_key = true;
        } else if (!strcmp(arg, "--size") && !have_size) {
            if (++i == solar_os_context_argc(ctx) || !solar_os_camera_parse_resolution(
                solar_os_context_argv(ctx, i), &photo.requested_width, &photo.requested_height))
                return ESP_ERR_INVALID_ARG;
            have_size = true;
        } else if (!have_source && arg[0] != '-') { source = arg; have_source = true; }
        else return ESP_ERR_INVALID_ARG;
    }
    if (!source[0] || strlen(source) >= sizeof(photo.source)) return ESP_ERR_INVALID_ARG;
    photo.remote = !strncmp(source, "rtsp://", 7);
    if (photo.remote) {
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
        if (have_size) return ESP_ERR_INVALID_ARG;
        esp_err_t error = solar_os_rtsp_url_normalize(source, photo.source, sizeof(photo.source));
        if (error != ESP_OK) return error;
        error = solar_os_rtsp_url_redact(photo.source, photo.label, sizeof(photo.label));
        if (error != ESP_OK) return error;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else {
        strlcpy(photo.source, source, sizeof(photo.source));
        strlcpy(photo.label, source, sizeof(photo.label));
    }
    esp_err_t error = solar_os_storage_resolve_path(directory, photo.directory, sizeof(photo.directory));
    if (error != ESP_OK) return error;
    if (strlen(photo.directory) + 20U >= sizeof(photo.directory)) return ESP_ERR_INVALID_SIZE;
    struct stat st;
    if (stat(photo.directory, &st) != 0 || !S_ISDIR(st.st_mode)) return ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

static esp_err_t start(solar_os_context_t *ctx)
{
    memset(&photo, 0, sizeof(photo)); photo.done = photo.save_done = true;
    esp_err_t error = parse(ctx);
    if (error != ESP_OK) return error;
    photo.mutex = xSemaphoreCreateMutex();
    if (!photo.mutex) return ESP_ERR_NO_MEM;
    solar_os_context_set_graphics_active(ctx, true);
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    photo.monochrome = solar_os_gfx_format(gfx) == SOLAR_OS_DISPLAY_FORMAT_MONO1;
    photo.direct = !photo.monochrome && solar_os_gfx_supports_frame_format(gfx, SOLAR_OS_DISPLAY_FORMAT_RGB565);
    const solar_os_display_rect_t rect = viewport(gfx);
    if (solar_os_gfx_width(gfx) < 40 || rect.width < 1 || rect.height < 8)
        return ESP_ERR_NOT_SUPPORTED;
    layout(ctx); refresh(ctx, true);
    photo.done = false;
    if (solar_os_task_create_pinned_external(worker, "photo", PHOTO_STACK, NULL,
        tskIDLE_PRIORITY + 1, &photo.worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        photo.done = true; return ESP_ERR_NO_MEM;
    }
    render(ctx);
    return ESP_OK;
}

static void stop(solar_os_context_t *ctx)
{
    __atomic_store_n(&photo.stop, true, __ATOMIC_RELAXED);
    refresh(ctx, false);
    solar_os_context_set_graphics_active(ctx, false);
    (void)solar_os_task_wait_done(photo.worker, &photo.done, SOLAR_OS_TASK_STOP_WAIT_MS);
    (void)solar_os_task_wait_done(photo.save_worker, &photo.save_done, SOLAR_OS_TASK_STOP_WAIT_MS);
}
static bool release_ready(void)
{ return __atomic_load_n(&photo.done, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&photo.save_done, __ATOMIC_ACQUIRE); }
static void cleanup(void)
{
    if (photo.worker) solar_os_task_delete_external(photo.worker);
    if (photo.save_worker) solar_os_task_delete_internal(photo.save_worker);
    frame_release(photo.pending); frame_release(photo.current); frame_release(photo.capture);
    if (photo.mutex) vSemaphoreDelete(photo.mutex);
}
static void suspend(solar_os_context_t *ctx)
{
    __atomic_store_n(&photo.suspended, true, __ATOMIC_RELAXED);
    refresh(ctx, false); solar_os_context_set_graphics_active(ctx, false);
}
static void resume(solar_os_context_t *ctx)
{
    solar_os_context_set_graphics_active(ctx, true);
    layout(ctx); refresh(ctx, true);
    __atomic_store_n(&photo.suspended, false, __ATOMIC_RELAXED);
    render(ctx);
}

static void capture(solar_os_context_t *ctx)
{
    xSemaphoreTake(photo.mutex, portMAX_DELAY);
    if (!photo.capture_busy && photo.current &&
        __atomic_load_n(&photo.save_done, __ATOMIC_ACQUIRE)) {
        if (photo.save_worker) {
            solar_os_task_delete_internal(photo.save_worker); photo.save_worker = NULL;
        }
        photo.capture = photo.current;
        __atomic_add_fetch(&photo.capture->refs, 1U, __ATOMIC_RELAXED);
        photo.capture_busy = true;
        photo.save_done = false;
        strlcpy(photo.message, "Saving...", sizeof(photo.message));
        /* Flash-backed VFS writes must run with an internal stack. Keep
         * preview/decoding in PSRAM and allocate this stack only on capture. */
        if (solar_os_task_create_pinned_internal(save_worker, "photo-save", PHOTO_SAVE_STACK,
            NULL, tskIDLE_PRIORITY + 1, &photo.save_worker, tskNO_AFFINITY,
            SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
            photo.save_done = true; photo.capture_busy = false;
            frame_release(photo.capture); photo.capture = NULL;
            strlcpy(photo.message, "Not enough memory to save", sizeof(photo.message));
        }
    }
    photo.dirty = true;
    xSemaphoreGive(photo.mutex);
    render(ctx);
}

static bool pointer(solar_os_context_t *ctx, const solar_os_input_pointer_event_t *event)
{
    solar_os_gfx_t *gfx = solar_os_context_gfx(ctx);
    const int width = solar_os_gfx_width(gfx), height = solar_os_gfx_height(gfx);
    if (event->mode == SOLAR_OS_INPUT_POINTER_ABSOLUTE) {
        photo.pointer_x = event->x; photo.pointer_y = event->y;
    } else {
        const int x = photo.pointer_x + event->delta_x, y = photo.pointer_y + event->delta_y;
        photo.pointer_x = x < 0 ? 0 : x >= width ? width - 1 : x;
        photo.pointer_y = y < 0 ? 0 : y >= height ? height - 1 : y;
    }
    if (photo.fullscreen || event->action != SOLAR_OS_INPUT_POINTER_PRESS ||
        !(event->buttons & SOLAR_OS_INPUT_POINTER_BUTTON_PRIMARY) ||
        solar_os_media_player_button_at(width, height, false, photo.pointer_x, photo.pointer_y) != 1)
        return false;
    capture(ctx);
    return true;
}

static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_RESUME) { resume(ctx); return true; }
    if (event->type == SOLAR_OS_EVENT_POINTER) return pointer(ctx, &event->data.pointer);
    if (event->type == SOLAR_OS_EVENT_TICK) {
        xSemaphoreTake(photo.mutex, portMAX_DELAY);
        const esp_err_t error = photo.error;
        xSemaphoreGive(photo.mutex);
        if (error != ESP_OK) {
            char message[96]; snprintf(message, sizeof(message), "photo: %s", esp_err_to_name(error));
            solar_os_context_finish(ctx, 1, message);
        } else render(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) return true;
    uint8_t key = (uint8_t)event->data.ch;
    if (key == '\r') key = SOLAR_OS_KEY_ENTER;
    if (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_APP_EXIT) {
        solar_os_context_finish(ctx, 0, NULL); return true;
    }
    if (key == 'f' || key == 'F') {
        photo.fullscreen = !photo.fullscreen; layout(ctx); render(ctx); return true;
    }
    if (key == photo.key) capture(ctx);
    return true;
}
static uint32_t tick_interval(void) { return 33; }

void solar_os_photo_complete(const char *const *tokens, size_t count,
    void (*emit)(const char *candidate, void *user), void *user)
{
    const char *source = "camera0";
    bool have_source = false, dir = false, key = false, size = false, fullscreen = false;
    for (size_t i = 1; i < count; i++) {
        const char *arg = tokens[i];
        if (!strcmp(arg, "--fullscreen")) { fullscreen = true; continue; }
        const bool option = !strcmp(arg, "--dir") || !strcmp(arg, "--key") || !strcmp(arg, "--size");
        if (!option) { if (have_source) return; source = arg; have_source = true; continue; }
        if (!strcmp(arg, "--dir")) dir = true;
        if (!strcmp(arg, "--key")) key = true;
        if (!strcmp(arg, "--size")) size = true;
        if (++i == count) {
            if (!strcmp(arg, "--key")) {
                const char *keys[] = {"ENTER", "SPACE", "F1", "F2", "UP", "DOWN", "LEFT", "RIGHT"};
                for (size_t j = 0; j < sizeof(keys) / sizeof(keys[0]); j++) emit(keys[j], user);
            } else if (!strcmp(arg, "--size")) {
                solar_os_stream_info_t info;
                if (solar_os_stream_get_info(source, &info) == ESP_OK && info.type == SOLAR_OS_STREAM_TYPE_VIDEO &&
                    info.video.width && info.video.height) {
                    char value[24]; snprintf(value, sizeof(value), "%ux%u", info.video.width, info.video.height);
                    emit(value, user);
                }
            }
            return;
        }
    }
    if (!have_source) {
        for (size_t i = 0; i < solar_os_stream_count(); i++) {
            solar_os_stream_info_t info;
            if (solar_os_stream_get(i, &info) && info.type == SOLAR_OS_STREAM_TYPE_VIDEO &&
                info.direction != SOLAR_OS_STREAM_DIRECTION_SINK && info.video.codec == SOLAR_OS_STREAM_VIDEO_JPEG)
                emit(info.id, user);
        }
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
        emit("rtsp://", user);
#endif
    }
    if (!dir) emit("--dir", user);
    if (!key) emit("--key", user);
    if (!size && strncmp(source, "rtsp://", 7)) emit("--size", user);
    if (!fullscreen) emit("--fullscreen", user);
}

const solar_os_app_t solar_os_photo_app = {
    .name = "photo", .summary = "live viewfinder and JPEG capture",
    .app_class = SOLAR_OS_APP_CLASS_GUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS,
    .start = start, .stop = stop, .suspend = suspend, .resume = resume, .event = event,
    .state_slot = &photo_state, .state_size = sizeof(photo_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = release_ready, .state_release_cleanup = cleanup,
    .requested_tick_interval_ms = tick_interval,
};
