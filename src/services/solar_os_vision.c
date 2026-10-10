#include "solar_os_vision.h"

#include <string.h>
#include "quirc.h"
#include "esp_timer.h"
#include "solar_os_memory.h"
#include "solar_os_task.h"

#define VISION_WORKER_STACK (20U * 1024U)

typedef struct {
    solar_os_raster_image_t *image;
    solar_os_raster_image_convert_options_t options;
    solar_os_vision_qr_results_t *results;
    struct quirc_code code;
    struct quirc_data data;
    volatile bool done;
    bool cancel;
    int64_t deadline_us;
    esp_err_t error;
} vision_work_t;

static bool vision_busy;

void solar_os_vision_qr_results_free(solar_os_vision_qr_results_t *results)
{
    if (!results) return;
    for (size_t i = 0; i < results->count; ++i)
        solar_os_memory_free(results->codes[i].payload);
    solar_os_memory_free(results);
}

static int vision_progress(void *user)
{
    vision_work_t *work = user;
    vTaskDelay(1); /* Recognition must also let idle/watchdog tasks run. */
    return __atomic_load_n(&work->cancel, __ATOMIC_ACQUIRE) ||
        esp_timer_get_time() >= work->deadline_us;
}

static void vision_run(vision_work_t *work)
{
    const int64_t start = esp_timer_get_time();
    struct quirc *recognizer = quirc_new();
    if (!recognizer) { work->error = ESP_ERR_NO_MEM; return; }
    const uint32_t w = work->options.output_width, h = work->options.output_height;
    if (quirc_resize(recognizer, w, h)) {
        work->error = ESP_ERR_NO_MEM;
        quirc_destroy(recognizer);
        return;
    }
    solar_os_vision_qr_results_t *result = work->results;
    uint8_t *gray = quirc_begin(recognizer, NULL, NULL);
    work->error = solar_os_raster_image_convert(work->image, &work->options,
                                               gray, (size_t)w * h, w);
    if (work->error != ESP_OK) goto done;
    result->preprocess_us = esp_timer_get_time() - start;
    if (vision_progress(work)) { work->error = ESP_ERR_TIMEOUT; goto done; }
    quirc_set_progress(recognizer, vision_progress, work);
    const int64_t detect_start = esp_timer_get_time();
    quirc_end(recognizer);
    result->detect_us = esp_timer_get_time() - detect_start;
    if (vision_progress(work)) { work->error = ESP_ERR_TIMEOUT; goto done; }
    const int64_t decode_start = esp_timer_get_time();
    const int candidates = quirc_count(recognizer);
    result->candidates = candidates;
    for (int i = 0; i < candidates; ++i) {
        if (vision_progress(work)) { work->error = ESP_ERR_TIMEOUT; goto done; }
        quirc_extract(recognizer, i, &work->code);
        quirc_decode_error_t error = quirc_decode(&work->code, &work->data);
        if (error == QUIRC_ERROR_DATA_ECC) {
            quirc_flip(&work->code);
            error = quirc_decode(&work->code, &work->data);
        }
        if (error != QUIRC_SUCCESS) { ++result->decode_failures; continue; }
        if (result->count == SOLAR_OS_VISION_QR_MAX) {
            result->truncated = true;
            break;
        }
        solar_os_vision_qr_code_t *code = &result->codes[result->count];
        code->length = work->data.payload_len;
        if (code->length) {
            code->payload = solar_os_memory_alloc(code->length,
                SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vision.qr.payload");
            if (!code->payload) { work->error = ESP_ERR_NO_MEM; goto done; }
            memcpy(code->payload, work->data.payload, code->length);
        }
        code->version = work->data.version;
        code->ecc_level = work->data.ecc_level;
        code->data_type = work->data.data_type;
        code->eci = work->data.eci;
        for (size_t j = 0; j < 4; ++j) {
            code->corners[j].x = work->options.x +
                (int64_t)work->code.corners[j].x * work->options.width / w;
            code->corners[j].y = work->options.y +
                (int64_t)work->code.corners[j].y * work->options.height / h;
        }
        ++result->count;
    }
    result->decode_us = esp_timer_get_time() - decode_start;
done:
    result->elapsed_us = esp_timer_get_time() - start;
    quirc_destroy(recognizer);
}

static void vision_worker(void *user)
{
    vision_work_t *work = user;
    vision_run(work);
    __atomic_store_n(&work->done, true, __ATOMIC_RELEASE);
    vTaskSuspend(NULL);
}

esp_err_t solar_os_vision_qrcodes(solar_os_raster_image_t *image,
    const solar_os_raster_image_convert_options_t *options,
    solar_os_vision_cancel_fn cancel, void *user,
    solar_os_vision_qr_results_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!image) return ESP_ERR_INVALID_ARG;
    const uint32_t iw = solar_os_raster_image_width(image);
    const uint32_t ih = solar_os_raster_image_height(image);
    solar_os_raster_image_convert_options_t opt = options ? *options :
        (solar_os_raster_image_convert_options_t){0};
    if (opt.x >= iw || opt.y >= ih) return ESP_ERR_INVALID_SIZE;
    if (!opt.width) opt.width = iw - opt.x;
    if (!opt.height) opt.height = ih - opt.y;
    if (!opt.output_width) opt.output_width = opt.width;
    if (!opt.output_height) opt.output_height = opt.height;
    opt.format = SOLAR_OS_RASTER_IMAGE_GRAY8;
    if (opt.width > iw - opt.x || opt.height > ih - opt.y ||
        opt.output_width > SOLAR_OS_VISION_MAX_WIDTH ||
        opt.output_height > SOLAR_OS_VISION_MAX_HEIGHT)
        return ESP_ERR_INVALID_SIZE;
    if (cancel && cancel(user)) return ESP_ERR_TIMEOUT;
    bool expected = false;
    if (!__atomic_compare_exchange_n(&vision_busy, &expected, true, false,
        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return ESP_ERR_INVALID_STATE;
    esp_err_t error = ESP_ERR_NO_MEM;
    vision_work_t *work = solar_os_memory_calloc(1, sizeof(*work),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vision.work");
    if (!work) goto unlock;
    work->results = solar_os_memory_calloc(1, sizeof(*work->results),
        SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "vision.qr.results");
    if (!work->results) goto cleanup;
    *work->results = (solar_os_vision_qr_results_t){
        .width = iw, .height = ih,
        .processed_width = opt.output_width, .processed_height = opt.output_height,
    };
    work->image = image; work->options = opt;
    work->deadline_us = esp_timer_get_time() + SOLAR_OS_VISION_TIMEOUT_MS * 1000LL;
    solar_os_raster_image_retain(image);
    TaskHandle_t task = NULL;
    /* In-memory processing only: the worker never initiates filesystem/flash
     * access. Its external stack includes quirc's ~9 KiB decode workspace. */
    if (solar_os_task_create_pinned_external(vision_worker, "vision-qr",
        VISION_WORKER_STACK, work, tskIDLE_PRIORITY + 1, &task,
        tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) == pdPASS) {
        bool cancelled = false;
        while (!__atomic_load_n(&work->done, __ATOMIC_ACQUIRE)) {
            if ((cancel && cancel(user)) || esp_timer_get_time() >= work->deadline_us) {
                cancelled = true;
                __atomic_store_n(&work->cancel, true, __ATOMIC_RELEASE);
            }
            vTaskDelay(1);
        }
        solar_os_task_delete_external(task);
        if ((cancel && cancel(user)) || esp_timer_get_time() >= work->deadline_us)
            cancelled = true;
        error = cancelled ? ESP_ERR_TIMEOUT : work->error;
        if (error == ESP_OK) { *out = work->results; work->results = NULL; }
    }
    solar_os_raster_image_release(image);
cleanup:
    solar_os_vision_qr_results_free(work->results);
    solar_os_memory_free(work);
unlock:
    __atomic_store_n(&vision_busy, false, __ATOMIC_RELEASE);
    return error;
}
