#include "solar_os_epd.h"

#include <string.h>
#include <stdio.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool solar_os_epd_time_due(const solar_os_epd_maintenance_t *m)
{
    if (!m || !m->interval_s || !m->recorded) return false;
    const int64_t now = esp_timer_get_time();
    return now >= m->last_cleanup_us &&
        now - m->last_cleanup_us >= (int64_t)m->interval_s * 1000000;
}

solar_os_epd_plan_t solar_os_epd_native_plan(const solar_os_epd_maintenance_t *m,
    bool changed, bool valid, bool forced, bool encoding_changed, bool count_due)
{
    solar_os_epd_reason_t reason = !valid ? SOLAR_OS_EPD_REASON_BASELINE :
        forced ? SOLAR_OS_EPD_REASON_EXPLICIT : encoding_changed ? SOLAR_OS_EPD_REASON_ENCODING :
        SOLAR_OS_EPD_REASON_NONE;
    if (reason != SOLAR_OS_EPD_REASON_NONE)
        return (solar_os_epd_plan_t){SOLAR_OS_EPD_FULL, reason};
    if (!changed) return (solar_os_epd_plan_t){SOLAR_OS_EPD_KEEP, SOLAR_OS_EPD_REASON_NONE};
    if (m && m->interval_s) {
        if (!m->recorded) reason = SOLAR_OS_EPD_REASON_BASELINE;
        else if (solar_os_epd_time_due(m)) reason = SOLAR_OS_EPD_REASON_TIME;
    } else if (count_due) reason = SOLAR_OS_EPD_REASON_COUNT;
    return (solar_os_epd_plan_t){reason == SOLAR_OS_EPD_REASON_NONE ?
        SOLAR_OS_EPD_PARTIAL : SOLAR_OS_EPD_FULL, reason};
}

void solar_os_epd_note_cleanup(solar_os_epd_maintenance_t *m, solar_os_epd_reason_t reason)
{
    m->last_cleanup_us = esp_timer_get_time();
    m->recorded = true;
    ++m->cleanups;
    m->last_reason = reason;
}

esp_err_t solar_os_epd_set_maintenance(solar_os_epd_maintenance_t *m, const char *mode)
{
    if (!m || !mode) return ESP_ERR_INVALID_ARG;
    const char key[] = "cleanup-interval=";
    if (strncmp(mode, key, sizeof(key)-1U)) return ESP_ERR_NOT_FOUND;
    const char *p = mode + sizeof(key)-1U;
    if (!*p) return ESP_ERR_INVALID_ARG;
    unsigned value = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9' || value > 86400U) return ESP_ERR_INVALID_ARG;
        value = value * 10U + (unsigned)(*p-'0');
    }
    if (value > 86400U) return ESP_ERR_INVALID_ARG;
    m->interval_s = value;
    return ESP_OK;
}

void solar_os_epd_append_maintenance(const solar_os_epd_maintenance_t *m, char *text, size_t size)
{
    static const char *const reasons[] = {"none", "baseline", "explicit", "encoding",
        "count", "time", "exposure", "recovery", "native"};
    const size_t used = strlen(text);
    if (used < size)
        snprintf(text+used, size-used, " cleanup-interval=%u cleanups=%u cleanup-reason=%s",
            (unsigned)m->interval_s, (unsigned)m->cleanups,
            (unsigned)m->last_reason < sizeof(reasons)/sizeof(*reasons) ? reasons[m->last_reason] : "unknown");
}

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
    const uint32_t cleanups = a->state->maintenance.cleanups;
    const esp_err_t err = a->baseline(a->context, a->worker->snapshot);
    *a->shadow_valid = err == ESP_OK;
    *a->needs_cleanup = err != ESP_OK;
    if (err == ESP_OK) {
        if (a->state->maintenance.cleanups == cleanups)
            solar_os_epd_note_cleanup(&a->state->maintenance, SOLAR_OS_EPD_REASON_RECOVERY);
        else a->state->maintenance.last_reason = SOLAR_OS_EPD_REASON_RECOVERY;
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
    solar_os_epd_append_maintenance(&s->maintenance, text, size);
}

bool solar_os_epd_cleanup_due(const solar_os_epd_t *s)
{
    /* A time deadline is serviced only by a changed target, never by an idle
     * worker or a duplicate submission. No timer wakes the panel. */
    if (s->maintenance.interval_s) return false;
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

bool solar_os_epd_mono_changed(const uint8_t *target, const uint8_t *previous,
    unsigned width, unsigned height)
{
    if (!target || !previous) return true;
    for (unsigned tile = 0; tile < (height+7U)/8U; ++tile) {
        const unsigned rows = height-tile*8U;
        const uint8_t mask = rows >= 8U ? 0xffU : (1U << rows)-1U;
        for (unsigned x = 0; x < width; ++x) {
            const size_t byte = (size_t)tile*width+x;
            if ((target[byte]^previous[byte]) & mask) return true;
        }
    }
    return false;
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
    const bool time_cleanup = fresh && *a->shadow_valid && !a->require_baseline &&
        s->maintenance.interval_s && (!s->maintenance.recorded || solar_os_epd_time_due(&s->maintenance)) &&
        solar_os_epd_mono_changed(target, a->shadow, a->width, a->height);
    if (a->require_baseline || !*a->shadow_valid || *a->needs_cleanup ||
        solar_os_epd_cleanup_due(s) || time_cleanup) {
        const solar_os_epd_reason_t reason = a->require_baseline || !*a->shadow_valid ||
            *a->needs_cleanup || (s->maintenance.interval_s && !s->maintenance.recorded) ?
            SOLAR_OS_EPD_REASON_BASELINE : time_cleanup ? SOLAR_OS_EPD_REASON_TIME :
            s->tuning.cleanup_adaptive && s->budget.peak_drive_cycles >= solar_os_epd_drive_limit(s) ?
            SOLAR_OS_EPD_REASON_EXPOSURE : SOLAR_OS_EPD_REASON_COUNT;
        const uint32_t cleanups = s->maintenance.cleanups;
        const esp_err_t err = a->baseline(a->context, target);
        if (err != ESP_OK) return err;
        if (s->maintenance.cleanups == cleanups) solar_os_epd_note_cleanup(&s->maintenance, reason);
        else s->maintenance.last_reason = reason;
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
