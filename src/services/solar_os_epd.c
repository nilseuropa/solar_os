#include "solar_os_epd.h"

#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

esp_err_t solar_os_epd_allocate_budget(solar_os_epd_t *s, size_t pixels,
    uint8_t **planes, size_t plane_bytes, const solar_os_epd_storage_ops_t *storage)
{
    if (!s || !planes || !storage || !storage->allocate || !storage->release ||
        !pixels || pixels > UINT32_MAX || pixels > SIZE_MAX / sizeof(uint32_t) || !plane_bytes)
        return ESP_ERR_INVALID_ARG;
    if (s->budget.pixels)
        return *planes && s->budget.count == pixels ? ESP_OK : ESP_ERR_INVALID_STATE;
    uint8_t *bits = storage->allocate(pixels, "epd.budget");
    uint32_t *active = storage->allocate(pixels * sizeof(*active), "epd.active");
    uint16_t *history = storage->allocate(pixels * sizeof(*history), "epd.history");
    uint8_t *masks = storage->allocate(plane_bytes, "epd.masks");
    if (!bits || !active || !history || !masks) {
        storage->release(bits); storage->release(active);
        storage->release(history); storage->release(masks);
        return ESP_ERR_NO_MEM;
    }
    /* Sizes and initial state have been validated before committing storage. */
    epd_pixel_budget_init(&s->budget, bits, pixels);
    epd_pixel_budget_set_active_storage(&s->budget, active, pixels);
    epd_pixel_budget_set_drive_history(&s->budget, history, pixels);
    *planes = masks;
    return ESP_OK;
}

void solar_os_epd_free_budget(solar_os_epd_t *s, uint8_t **planes,
    const solar_os_epd_storage_ops_t *storage)
{
    storage->release(s->budget.active);
    storage->release(s->budget.drive_history);
    storage->release(s->budget.pixels);
    storage->release(*planes);
    memset(&s->budget, 0, sizeof(s->budget));
    *planes = NULL;
}

esp_err_t solar_os_epd_recover(const solar_os_epd_adapter_t *a)
{
    if (!a->worker || !a->worker->snapshot || !a->worker->latest)
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(a->worker->lock, portMAX_DELAY);
    memcpy(a->worker->snapshot, a->worker->latest, a->frame_bytes);
    xSemaphoreGive(a->worker->lock);
    const esp_err_t err = a->baseline(a->context, a->worker->snapshot);
    *a->shadow_valid = err == ESP_OK;
    *a->needs_cleanup = err != ESP_OK;
    if (err == ESP_OK) {
        memcpy(a->shadow, a->worker->snapshot, a->frame_bytes);
        /* Keep the aborted pulse budget as diagnostics. The adapter has
         * switched to native mode; re-entering pulse mode must reseed it.
         * A failed physical drive must never appear to consume its budget. */
        *a->mask_dirty = true;
        if (a->window_valid) *a->window_valid = false;
    }
    return err;
}

static bool target_waiting(const solar_os_epd_adapter_t *a, const uint8_t *target)
{
    if (!a->asynchronous || !a->worker || !a->worker->task) return false;
    xSemaphoreTake(a->worker->lock, portMAX_DELAY);
    const bool changed = a->worker->has_target &&
        memcmp(a->worker->latest, target, a->frame_bytes) != 0;
    xSemaphoreGive(a->worker->lock);
    return changed;
}

unsigned solar_os_epd_drive_limit(const solar_os_epd_t *s)
{
    const epd_pulse_tuning_t *t = &s->tuning;
    const unsigned extra = t->black_redrives > t->white_redrives ?
        t->black_redrives : t->white_redrives;
    return t->cleanup_every * (t->drive_cycles + extra);
}

void solar_os_epd_append_mode(const solar_os_epd_t *s, char *text, size_t size)
{
    const size_t used = strlen(text);
    if (used < size)
        snprintf(text + used, size - used,
            " black-redrives=%u white-redrives=%u changed-drive-frames=%u pulse-transfer-us=%lld pulse-busy-us=%lld",
            s->tuning.black_redrives, s->tuning.white_redrives, s->tuning.pulse_frames,
            (long long)s->last_transfer_us, (long long)s->last_busy_us);
}

bool solar_os_epd_cleanup_due(const solar_os_epd_t *s)
{
    const epd_pulse_tuning_t *t = &s->tuning;
    return s->updates >= t->cleanup_every * (t->cleanup_adaptive ? 4U : 1U) ||
        (t->cleanup_adaptive && s->budget.peak_drive_cycles >= solar_os_epd_drive_limit(s));
}

void solar_os_epd_seed(solar_os_epd_t *s, const uint8_t *target,
                       unsigned width, unsigned height, bool tile_budget)
{
    epd_pixel_budget_reset(&s->budget);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const size_t byte = (size_t)(y / 8U) * width + x;
            const size_t p = tile_budget ? byte * 8U + y % 8U : (size_t)y * width + x;
            epd_pixel_budget_seed(&s->budget, p, (target[byte] & (1U << (y % 8U))) != 0);
        }
    }
}

bool solar_os_epd_retarget(const solar_os_epd_adapter_t *a, const uint8_t *target)
{
    bool changed = false;
    for (unsigned tile = 0; tile < (a->height + 7U) / 8U; ++tile) {
        const unsigned rows = a->height - tile * 8U;
        const uint8_t visible = rows >= 8U ? 0xffU : (1U << rows) - 1U;
        for (unsigned x = 0; x < a->width; ++x) {
            const size_t byte = (size_t)tile * a->width + x;
            const uint8_t bits = (target[byte] ^ a->shadow[byte]) & visible;
            if (!bits) continue;
            changed = true;
            for (unsigned bit = 0; bit < 8U; ++bit) {
                if (!(bits & (1U << bit))) continue;
                const size_t p = a->tile_budget ? byte * 8U + bit :
                    (size_t)(tile * 8U + bit) * a->width + x;
                const bool white = (target[byte] & (1U << bit)) != 0;
                epd_pixel_budget_target(&a->state->budget, p, white,
                    epd_pulse_target_cycles(&a->state->tuning, white));
            }
        }
    }
    memcpy(a->shadow, target, a->frame_bytes);
    if (changed) *a->mask_dirty = true;
    return changed;
}

esp_err_t solar_os_epd_prepare(const solar_os_epd_adapter_t *a,
                              const uint8_t *target, bool fresh, bool *baseline)
{
    solar_os_epd_t *s = a->state;
    *baseline = false;
    if (a->allocate) {
        const esp_err_t err = a->allocate(a->context);
        if (err != ESP_OK) return err;
    }
    if (a->require_baseline || !*a->shadow_valid || *a->needs_cleanup ||
        solar_os_epd_cleanup_due(s)) {
        const esp_err_t err = a->baseline(a->context, target);
        if (err != ESP_OK) return err;
        memcpy(a->shadow, target, a->frame_bytes);
        solar_os_epd_seed(s, target, a->width, a->height, a->tile_budget);
        *a->shadow_valid = true;
        *a->needs_cleanup = false;
        *a->mask_dirty = true;
        if (a->window_valid) *a->window_valid = false;
        s->updates = 0;
        s->last_frame_start = 0;
        *baseline = true;
        return ESP_OK;
    }
    if (fresh && solar_os_epd_retarget(a, target)) {
        if (!a->asynchronous && s->last_frame_start) {
            const int64_t due = s->last_frame_start + (int64_t)a->period_ms * 1000;
            int64_t remaining;
            while ((remaining = due - esp_timer_get_time()) > 0) {
                TickType_t ticks = pdMS_TO_TICKS((remaining + 999) / 1000);
                vTaskDelay(ticks ? ticks : 1);
            }
        }
        s->last_frame_start = esp_timer_get_time();
        if (a->count_on_accept) ++s->updates;
    }
    return ESP_OK;
}

esp_err_t solar_os_epd_step(const solar_os_epd_adapter_t *a,
                           const uint8_t *target, bool fresh,
                           bool *pending, bool *scanned)
{
    bool baseline;
    *pending = *scanned = false;
    esp_err_t err = solar_os_epd_prepare(a, target, fresh, &baseline);
    if (err == ESP_OK && !baseline && a->state->budget.pending) {
        err = a->drive(a->context);
        *scanned = err == ESP_OK;
    }
    if (err == ESP_OK && !a->state->budget.pending && !target_waiting(a, target))
        err = a->idle(a->context);
    if (err == ESP_OK)
        *pending = a->state->budget.pending != 0 || solar_os_epd_cleanup_due(a->state);
    return err;
}

esp_err_t solar_os_epd_flush(const solar_os_epd_adapter_t *a, const uint8_t *target)
{
    bool pending, scanned;
    bool fresh = true;
    do {
        const esp_err_t err = solar_os_epd_step(a, target, fresh, &pending, &scanned);
        if (err != ESP_OK) return err;
        fresh = false;
    } while (pending);
    return ESP_OK;
}
