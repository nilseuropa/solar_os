#include "solar_os_script_media.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_memory.h"
#include "solar_os_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
#include "solar_os_task.h"
#endif

typedef struct {
    uint32_t id, frame_id;
    bool snapshot, frame_leased;
    solar_os_stream_handle_t stream;
    solar_os_script_media_frame_t frame;
} media_source_t;

struct solar_os_script_media {
    char owner[SOLAR_OS_STREAM_OWNER_MAX];
    solar_os_script_media_cancel_fn cancel;
    void *user;
    media_source_t sources[SOLAR_OS_SCRIPT_MEDIA_STREAMS];
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    uint32_t rtsp_id, rtsp_frame_id;
    solar_os_rtsp_client_t *rtsp;
    TaskHandle_t worker;
    volatile bool done;
    solar_os_script_media_frame_t rtsp_frame;
#endif
};

/* Keep IDs representable in MicroPython builds without long integers. Never
 * wrap: exhaustion is preferable to accidentally reviving a stale handle. */
static uint32_t next_id;
bool solar_os_script_media_option_valid(solar_os_stream_type_t type, const char *key)
{
    if (!key) return false;
    if (!strcmp(key, "direction") || !strcmp(key, "timeout_ms")) return true;
    if (type == SOLAR_OS_STREAM_TYPE_VIDEO)
        return !strcmp(key, "width") || !strcmp(key, "height") || !strcmp(key, "jpeg_quality");
    if (type == SOLAR_OS_STREAM_TYPE_AUDIO)
        return !strcmp(key, "sample_rate") || !strcmp(key, "channels") || !strcmp(key, "frames_per_block");
    return false;
}
static uint32_t new_id(void)
{
    uint32_t previous = __atomic_load_n(&next_id, __ATOMIC_RELAXED);
    while (previous < 0x1fffffffU) {
        if (__atomic_compare_exchange_n(&next_id, &previous, previous + 1U,
            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return previous + 1U;
    }
    return 0U;
}

static bool cancelled(solar_os_script_media_t *s)
{
    return s->cancel != NULL && s->cancel(s->user);
}

static media_source_t *source(solar_os_script_media_t *s, uint32_t id)
{
    if (s != NULL && id != 0U) {
        for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i)
            if (s->sources[i].id == id) return &s->sources[i];
    }
    return NULL;
}

esp_err_t solar_os_script_media_create(const char *owner,
    solar_os_script_media_cancel_fn cancel, void *user, solar_os_script_media_t **out)
{
    if (owner == NULL || owner[0] == '\0' || strlen(owner) >= SOLAR_OS_STREAM_OWNER_MAX || !out)
        return ESP_ERR_INVALID_ARG;
    *out = solar_os_memory_calloc(1, sizeof(**out),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "script.media");
    if (!*out) return ESP_ERR_NO_MEM;
    strcpy((*out)->owner, owner);
    (*out)->cancel = cancel; (*out)->user = user;
    return ESP_OK;
}

esp_err_t solar_os_script_media_open(solar_os_script_media_t *s,
    const char *id, const solar_os_stream_open_options_t *options, uint32_t *handle)
{
    if (!s || !id || !options || !handle || options->timeout_ms > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX)
        return ESP_ERR_INVALID_ARG;
    *handle = 0;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i) {
        media_source_t *p = &s->sources[i];
        if (p->id) continue;
        const uint32_t key = new_id();
        if (!key) return ESP_ERR_NO_MEM;
        p->stream = (solar_os_stream_handle_t)SOLAR_OS_STREAM_HANDLE_INIT;
        esp_err_t err = solar_os_stream_open_ex(id, s->owner, options, &p->stream);
        if (err != ESP_OK) return err;
        p->id = key; p->snapshot = false; *handle = key;
        return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t solar_os_script_media_release(solar_os_script_media_t *s, uint32_t id)
{
    if (!s || !id) return ESP_ERR_INVALID_ARG;
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (s->rtsp_frame_id == id) {
        solar_os_rtsp_client_release_video(s->rtsp);
        s->rtsp_frame_id = 0; memset(&s->rtsp_frame, 0, sizeof(s->rtsp_frame));
        return ESP_OK;
    }
#endif
    for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i) {
        media_source_t *p = &s->sources[i];
        if (p->frame_id != id) continue;
        if (p->frame_leased) {
            esp_err_t err = solar_os_stream_release_frame(&p->stream, &p->frame.jpeg);
            if (err != ESP_OK) return err;
            p->frame_leased = false;
            memset(&p->frame, 0, sizeof(p->frame));
        }
        if (p->snapshot) {
            /* On failed stop, retain the ID as a release-only token so callers
             * can retry releasing the source without touching freed bytes. */
            esp_err_t err = solar_os_stream_close_ex(&p->stream);
            if (err == ESP_OK) memset(p, 0, sizeof(*p));
            return err;
        }
        p->frame_id = 0;
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_script_media_close(solar_os_script_media_t *s, uint32_t id)
{
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (s && id && s->rtsp_id == id) {
        if (s->rtsp_frame_id) (void)solar_os_script_media_release(s, s->rtsp_frame_id);
        solar_os_rtsp_client_cancel(s->rtsp);
        /* Do not delete a running network task or free buffers it still owns.
         * Cancel is checked by native networking; wait cooperatively to reap. */
        while (!__atomic_load_n(&s->done, __ATOMIC_ACQUIRE)) vTaskDelay(1);
        solar_os_task_delete_external(s->worker);
        esp_err_t err = solar_os_rtsp_client_destroy(s->rtsp);
        if (err != ESP_OK) return err;
        s->rtsp = NULL; s->worker = NULL; s->rtsp_id = 0;
        return ESP_OK;
    }
#endif
    media_source_t *p = source(s, id);
    if (!p) return ESP_ERR_INVALID_ARG;
    /* Explicit source close releases the frame before the checked stop. */
    p->snapshot = false;
    if (p->frame_id) {
        esp_err_t err = solar_os_script_media_release(s, p->frame_id);
        if (err != ESP_OK) return err;
    }
    esp_err_t err = solar_os_stream_close_ex(&p->stream);
    if (err == ESP_OK) memset(p, 0, sizeof(*p));
    return err;
}

esp_err_t solar_os_script_media_close_all(solar_os_script_media_t *s)
{
    if (!s) return ESP_OK;
    esp_err_t error = ESP_OK;
    for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i) {
        if (!s->sources[i].id) continue;
        const esp_err_t err = solar_os_script_media_close(s, s->sources[i].id);
        if (err != ESP_OK) error = err;
    }
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (s->rtsp_id) {
        const esp_err_t err = solar_os_script_media_close(s, s->rtsp_id);
        if (err != ESP_OK) error = err;
    }
#endif
    return error;
}

void solar_os_script_media_destroy(solar_os_script_media_t *s)
{
    if (!s) return;
    /* Failed driver close retains the live lease: never free its frame object.
     * Providers should make release/stop reliable; preserve safety on failure. */
    esp_err_t err = ESP_OK;
    for (unsigned retry = 0; retry < 3; ++retry) {
        err = solar_os_script_media_close_all(s);
        if (err == ESP_OK) { solar_os_memory_free(s); return; }
    }
    SOLAR_OS_LOGE("script.media", "provider teardown failed: %s; retaining live lease state", esp_err_to_name(err));
}

esp_err_t solar_os_script_media_read(solar_os_script_media_t *s, uint32_t id,
    void *data, size_t size, uint32_t timeout, size_t *length)
{
    media_source_t *p = source(s, id);
    if (!p || !data || !length || !size || size > SOLAR_OS_SCRIPT_MEDIA_TRANSFER_MAX ||
        timeout > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX) return ESP_ERR_INVALID_ARG;
    *length = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
    do {
        if (cancelled(s)) return ESP_ERR_TIMEOUT;
        int64_t left_us = deadline - esp_timer_get_time();
        uint32_t slice = left_us > 0 ? (uint32_t)((left_us + 999) / 1000) : 0;
        if (slice > 50) slice = 50;
        esp_err_t err = solar_os_stream_read(&p->stream, data, size, slice, length);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return err;
        if (*length || esp_timer_get_time() >= deadline) return err;
        vTaskDelay(pdMS_TO_TICKS(1) ? pdMS_TO_TICKS(1) : 1);
    } while (true);
}

esp_err_t solar_os_script_media_stream_info(solar_os_script_media_t *s,
    uint32_t id, solar_os_stream_info_t *info)
{
    media_source_t *p = source(s, id);
    if (!p || !info) return ESP_ERR_INVALID_ARG;
    esp_err_t err = solar_os_stream_get_info(p->stream.id, info);
    if (err != ESP_OK) return err;
    info->direction = p->stream.direction;
    if (info->type == SOLAR_OS_STREAM_TYPE_VIDEO) info->video = p->stream.video;
    if (info->type == SOLAR_OS_STREAM_TYPE_AUDIO) info->audio = p->stream.audio;
    return ESP_OK;
}

esp_err_t solar_os_script_media_write(solar_os_script_media_t *s, uint32_t id,
    const void *data, size_t size, uint32_t timeout, size_t *length)
{
    media_source_t *p = source(s, id);
    if (!p || !data || !length || !size || size > SOLAR_OS_SCRIPT_MEDIA_TRANSFER_MAX ||
        timeout > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX) return ESP_ERR_INVALID_ARG;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    return solar_os_stream_write(&p->stream, data, size, timeout, length);
}

esp_err_t solar_os_script_media_scalar(solar_os_script_media_t *s, uint32_t id,
    const solar_os_stream_read_options_t *options, float *value)
{
    media_source_t *p = source(s, id);
    if (!p || !options || !value || options->timeout_ms > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX ||
        options->window_ms > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX) return ESP_ERR_INVALID_ARG;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    return solar_os_stream_read_scalar(&p->stream, options, value);
}

esp_err_t solar_os_script_media_acquire(solar_os_script_media_t *s,
    uint32_t id, uint32_t *frame)
{
    media_source_t *p = source(s, id);
    if (!p || !frame) return ESP_ERR_INVALID_ARG;
    *frame = 0;
    if (p->frame_id) return ESP_ERR_INVALID_STATE;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    const uint32_t key = new_id();
    if (!key) return ESP_ERR_NO_MEM;
    esp_err_t err = solar_os_stream_acquire_frame(&p->stream, &p->frame.jpeg);
    if (err == ESP_OK) {
        p->frame_leased = true;
        p->frame_id = key;
        if (!p->frame.jpeg.data || !p->frame.jpeg.length ||
            p->frame.jpeg.length > SOLAR_OS_SCRIPT_MEDIA_FRAME_MAX) {
            (void)solar_os_script_media_release(s, key);
            return ESP_ERR_INVALID_SIZE;
        }
        *frame = key;
    }
    return err;
}

esp_err_t solar_os_script_media_frame(solar_os_script_media_t *s,
    uint32_t id, solar_os_script_media_frame_t *info)
{
    if (!s || !id || !info) return ESP_ERR_INVALID_ARG;
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
    if (s->rtsp_frame_id == id) { *info = s->rtsp_frame; return ESP_OK; }
#endif
    for (size_t i = 0; i < SOLAR_OS_SCRIPT_MEDIA_STREAMS; ++i)
        if (s->sources[i].frame_id == id) {
            if (!s->sources[i].frame.jpeg.data) return ESP_ERR_INVALID_STATE;
            *info = s->sources[i].frame; return ESP_OK;
        }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t solar_os_script_media_save(solar_os_script_media_t *s,
    uint32_t id, const char *path, int *file_errno)
{
    if (file_errno) *file_errno = 0;
    if (!path || !*path) return ESP_ERR_INVALID_ARG;
    solar_os_script_media_frame_t frame;
    esp_err_t err = solar_os_script_media_frame(s, id, &frame);
    if (err != ESP_OK) return err;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    errno = 0;
    FILE *f = fopen(path, "wb");
    if (!f) {
        if (file_errno) *file_errno = errno ? errno : EIO;
        return ESP_FAIL;
    }
    errno = 0;
    const size_t written = fwrite(frame.jpeg.data, 1, frame.jpeg.length, f);
    int failure = written == frame.jpeg.length ? 0 : (errno ? errno : EIO);
    errno = 0;
    if (fclose(f) != 0 && !failure) failure = errno ? errno : EIO;
    if (file_errno) *file_errno = failure;
    return failure ? ESP_FAIL : ESP_OK;
}

esp_err_t solar_os_script_media_snapshot(solar_os_script_media_t *s,
    const char *id, uint16_t width, uint16_t height, uint8_t quality, uint32_t *frame)
{
    if (!frame || quality > 63U) return ESP_ERR_INVALID_ARG;
    *frame = 0;
    const solar_os_stream_open_options_t options = {
        .direction = SOLAR_OS_STREAM_DIRECTION_SOURCE,
        .requested_video = {.codec = SOLAR_OS_STREAM_VIDEO_JPEG,
            .width = width, .height = height, .jpeg_quality = quality},
    };
    uint32_t handle;
    esp_err_t err = solar_os_script_media_open(s, id, &options, &handle);
    if (err != ESP_OK) return err;
    err = solar_os_script_media_acquire(s, handle, frame);
    if (err != ESP_OK) { (void)solar_os_script_media_close(s, handle); return err; }
    source(s, handle)->snapshot = true;
    return ESP_OK;
}

#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
static void network_worker(void *user)
{
    solar_os_script_media_t *s = user;
    (void)solar_os_rtsp_client_run(s->rtsp);
    __atomic_store_n(&s->done, true, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}

esp_err_t solar_os_script_media_rtsp_open(solar_os_script_media_t *s,
    const char *url, bool video, bool audio, uint32_t *handle)
{
    if (!s || !handle) return ESP_ERR_INVALID_ARG;
    *handle = 0;
    if (s->rtsp_id) return ESP_ERR_INVALID_STATE;
    if (cancelled(s)) return ESP_ERR_TIMEOUT;
    const uint32_t id = new_id();
    if (!id) return ESP_ERR_NO_MEM;
    const solar_os_rtsp_client_options_t options = {.video = video, .audio = audio};
    esp_err_t err = solar_os_rtsp_client_create(url, &options, &s->rtsp);
    if (err != ESP_OK) return err;
    s->done = false;
    if (solar_os_task_create_pinned_external(network_worker, "script-rtsp", 8192, s,
        tskIDLE_PRIORITY + 2, &s->worker, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        (void)solar_os_rtsp_client_destroy(s->rtsp); s->rtsp = NULL;
        return ESP_ERR_NO_MEM;
    }
    s->rtsp_id = id; *handle = id;
    return ESP_OK;
}

esp_err_t solar_os_script_media_rtsp_status(solar_os_script_media_t *s,
    uint32_t id, solar_os_rtsp_client_status_t *status, bool *ended)
{
    if (!s || !id || s->rtsp_id != id || !status || !ended) return ESP_ERR_INVALID_ARG;
    *ended = __atomic_load_n(&s->done, __ATOMIC_ACQUIRE);
    solar_os_rtsp_client_status(s->rtsp, status);
    return ESP_OK;
}

esp_err_t solar_os_script_media_rtsp_read(solar_os_script_media_t *s,
    uint32_t id, uint32_t timeout, uint32_t *frame)
{
    if (!s || !id || s->rtsp_id != id || !frame || timeout > SOLAR_OS_SCRIPT_MEDIA_TIMEOUT_MAX)
        return ESP_ERR_INVALID_ARG;
    *frame = 0;
    if (s->rtsp_frame_id) return ESP_ERR_INVALID_STATE;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
    do {
        if (cancelled(s)) { solar_os_rtsp_client_cancel(s->rtsp); return ESP_ERR_TIMEOUT; }
        solar_os_rtp_jpeg_frame_t jpeg;
        uint64_t arrived;
        if (solar_os_rtsp_client_take_video(s->rtsp, &jpeg, &arrived)) {
            const uint32_t key = new_id();
            if (!key) { solar_os_rtsp_client_release_video(s->rtsp); return ESP_ERR_NO_MEM; }
            s->rtsp_frame = (solar_os_script_media_frame_t){
                .jpeg = {.data = jpeg.data, .length = jpeg.length,
                    .width = jpeg.width, .height = jpeg.height, .timestamp_us = arrived},
                .network = true, .rtp_timestamp = jpeg.timestamp, .arrived_us = arrived,
            };
            s->rtsp_frame_id = key; *frame = key;
            return ESP_OK;
        }
        if (__atomic_load_n(&s->done, __ATOMIC_ACQUIRE) || esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(10) ? pdMS_TO_TICKS(10) : 1);
    } while (true);
    return ESP_OK;
}

esp_err_t solar_os_script_media_rtsp_lateness(solar_os_script_media_t *s,
    uint32_t id, uint32_t frame, int64_t *lateness)
{
    if (!s || !id || s->rtsp_id != id || !frame || s->rtsp_frame_id != frame || !lateness)
        return ESP_ERR_INVALID_ARG;
    *lateness = solar_os_rtsp_client_video_lateness(s->rtsp,
        s->rtsp_frame.rtp_timestamp, s->rtsp_frame.arrived_us);
    return ESP_OK;
}
#endif
