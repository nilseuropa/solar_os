#include "epd_scan_worker.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct test_scan_task {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    unsigned notifications;
    void (*fn)(void *);
    void *arg;
};
static _Thread_local TaskHandle_t current;
static TaskHandle_t tasks[8];
static unsigned task_count;
static bool fail_create;
int64_t esp_timer_get_time(void) {
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
static void sleep_ms(unsigned ms) {
    struct timespec delay = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&delay, NULL);
}
static void *task_entry(void *arg) {
    current = arg; current->fn(current->arg); return NULL;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task) {
    (void)name; (void)stack; (void)priority;
    if (fail_create) return 0;
    TaskHandle_t t = calloc(1, sizeof(*t)); assert(t && task_count < 8);
    pthread_mutex_init(&t->lock, NULL); pthread_cond_init(&t->wake, NULL);
    t->fn = fn; t->arg = arg; tasks[task_count++] = t; *task = t;
    assert(pthread_create(&t->thread, NULL, task_entry, t) == 0); return pdPASS;
}
BaseType_t xTaskNotifyGive(TaskHandle_t t) {
    pthread_mutex_lock(&t->lock); ++t->notifications;
    pthread_cond_signal(&t->wake); pthread_mutex_unlock(&t->lock); return pdPASS;
}
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t timeout) {
    TaskHandle_t t = current; assert(t); pthread_mutex_lock(&t->lock);
    if (timeout == portMAX_DELAY) {
        while (!t->notifications) pthread_cond_wait(&t->wake, &t->lock);
    } else {
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += timeout / 1000; until.tv_nsec += (timeout % 1000) * 1000000L;
        if (until.tv_nsec >= 1000000000L) { ++until.tv_sec; until.tv_nsec -= 1000000000L; }
        while (!t->notifications)
            if (pthread_cond_timedwait(&t->wake, &t->lock, &until) == ETIMEDOUT) break;
    }
    unsigned n = t->notifications;
    if (clear) t->notifications = 0; else if (n) --t->notifications;
    pthread_mutex_unlock(&t->lock); return n;
}
void vTaskDelete(TaskHandle_t task) { assert(task == NULL); pthread_exit(NULL); }
static void reap(void) {
    for (unsigned i = 0; i < task_count; ++i) {
        pthread_join(tasks[i]->thread, NULL);
        pthread_cond_destroy(&tasks[i]->wake); pthread_mutex_destroy(&tasks[i]->lock);
        free(tasks[i]);
    }
    task_count = 0;
}
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    bool blocked, release, fail;
    unsigned calls, remaining, maintenance_calls;
    uint8_t seen[16];
    int64_t at_us[16];
} fixture_t;
static esp_err_t process(void *arg, const uint8_t *frame, bool fresh, bool *pending, bool *scanned) {
    fixture_t *f = arg; pthread_mutex_lock(&f->lock);
    if (fresh) f->remaining = 3;
    if (!f->remaining) {
        ++f->maintenance_calls;
        *pending = false; *scanned = false;
        pthread_mutex_unlock(&f->lock); return ESP_OK;
    }
    assert(f->remaining && f->calls < 16);
    f->at_us[f->calls] = esp_timer_get_time();
    f->seen[f->calls++] = frame[0];
    if (f->calls == 1) {
        f->blocked = true; pthread_cond_broadcast(&f->wake);
        while (!f->release) pthread_cond_wait(&f->wake, &f->lock);
        assert(frame[0] == 1); /* Latest submissions cannot overwrite this snapshot. */
    }
    --f->remaining; *pending = f->remaining != 0; *scanned = !f->fail;
    const esp_err_t err = f->fail ? ESP_FAIL : ESP_OK;
    pthread_mutex_unlock(&f->lock); return err;
}
static void begin(epd_scan_worker_t *w, fixture_t *f) {
    memset(w, 0, sizeof(*w)); memset(f, 0, sizeof(*f));
    pthread_mutex_init(&f->lock, NULL); pthread_cond_init(&f->wake, NULL);
    assert(epd_scan_worker_init(w, 4, process, f) == ESP_OK);
    assert(epd_scan_worker_set_period(w, 30) == ESP_OK);
    uint8_t image[4] = {1};
    assert(epd_scan_worker_submit(w, image) == ESP_ERR_INVALID_STATE);
    epd_scan_worker_enable(w, true);
    assert(epd_scan_worker_submit(w, image) == ESP_OK);
    memset(image, 99, sizeof(image));
    const int64_t deadline = esp_timer_get_time() + 2000000;
    for (;;) {
        pthread_mutex_lock(&f->lock); bool ready = f->blocked; pthread_mutex_unlock(&f->lock);
        if (ready) break;
        assert(esp_timer_get_time() < deadline); sleep_ms(1);
    }
}
static void release(fixture_t *f) {
    pthread_mutex_lock(&f->lock); f->release = true;
    pthread_cond_broadcast(&f->wake); pthread_mutex_unlock(&f->lock);
}
static void wait_scans(epd_scan_worker_t *w, unsigned count) {
    const int64_t deadline = esp_timer_get_time() + 2000000;
    while (epd_scan_worker_stats(w).scans < count) {
        assert(esp_timer_get_time() < deadline); sleep_ms(1);
    }
}
static void finish(epd_scan_worker_t *w, fixture_t *f) {
    epd_scan_worker_deinit(w); reap();
    pthread_cond_destroy(&w->lock_storage.wake); pthread_mutex_destroy(&w->lock_storage.lock);
    pthread_cond_destroy(&w->done_storage.wake); pthread_mutex_destroy(&w->done_storage.lock);
    pthread_cond_destroy(&f->wake); pthread_mutex_destroy(&f->lock);
}
static void test_busy_submission_and_coalescing(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    uint8_t image[4] = {2}; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    image[0] = 3; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    sleep_ms(35); /* The next new-image slot is due when the callback returns. */
    image[0] = 99; release(&f); wait_scans(&w, 4);
    epd_scan_stats_t stats = epd_scan_worker_stats(&w);
    assert(stats.submitted == 3 && stats.coalesced == 1 && stats.scans == 4);
    assert(stats.frames == 2 && stats.last_frame_interval_us >= 30000);
    assert(f.seen[0] == 1 && f.seen[1] == 3 && f.seen[2] == 3 && f.seen[3] == 3);
    finish(&w, &f);
}
static void test_redrives_between_image_slots(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    assert(epd_scan_worker_set_period(&w, 200) == ESP_OK);
    uint8_t image[4] = {2}; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    release(&f); wait_scans(&w, 3);
    epd_scan_stats_t stats = epd_scan_worker_stats(&w);
    assert(stats.frames == 1 && stats.scans == 3);
    assert(f.seen[1] == 1 && f.seen[2] == 1);
    assert(f.at_us[2] - f.at_us[0] < 200000);
    /* A faster second edit replaces the queued target before its image slot. */
    image[0] = 3; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    wait_scans(&w, 6);
    stats = epd_scan_worker_stats(&w);
    assert(stats.frames == 2 && stats.last_frame_interval_us >= 200000);
    assert(stats.coalesced == 1 && f.seen[3] == 3 && f.seen[5] == 3);
    /* Repeated identical presentations are not new images or new drives. */
    assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    sleep_ms(220);
    stats = epd_scan_worker_stats(&w);
    assert(stats.frames == 2 && stats.scans == 6);
    pthread_mutex_lock(&f.lock);
    assert(f.maintenance_calls == 1); /* Identical frames still service tuning/cleanup. */
    pthread_mutex_unlock(&f.lock);
    /* A mode/power generation change must reseed even an identical image. */
    epd_scan_worker_enable(&w, false); epd_scan_worker_enable(&w, true);
    assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    wait_scans(&w, 9);
    stats = epd_scan_worker_stats(&w);
    assert(stats.frames == 3 && stats.last_frame_interval_us == 0);
    finish(&w, &f);
}
static void test_new_image_interrupts_unfinished_redrives(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    uint8_t image[4] = {2}; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    sleep_ms(35);
    release(&f); wait_scans(&w, 4);
    assert(f.seen[0] == 1 && f.seen[1] == 2 && f.seen[3] == 2);
    assert(epd_scan_worker_stats(&w).frames == 2);
    finish(&w, &f);
}
static void test_disable_discards_inflight_budget(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    epd_scan_worker_enable(&w, false); epd_scan_worker_enable(&w, true);
    release(&f); wait_scans(&w, 1); sleep_ms(80);
    assert(epd_scan_worker_stats(&w).scans == 1);
    uint8_t image[4] = {4}; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    wait_scans(&w, 4); assert(f.seen[1] == 4); finish(&w, &f);
}
static void *stopper(void *arg) { epd_scan_worker_deinit(arg); return NULL; }
static void test_error_drops_queued_target(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    uint8_t image[4] = {2}; assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    pthread_mutex_lock(&f.lock); f.fail = true; pthread_mutex_unlock(&f.lock);
    release(&f);
    const int64_t deadline = esp_timer_get_time() + 2000000;
    for (;;) {
        xSemaphoreTake(w.lock, portMAX_DELAY); bool enabled = w.enabled; xSemaphoreGive(w.lock);
        if (!enabled) break;
        assert(esp_timer_get_time() < deadline); sleep_ms(1);
    }
    assert(epd_scan_worker_stats(&w).scans == 0);
    assert(epd_scan_worker_submit(&w, image) == ESP_ERR_INVALID_STATE);
    pthread_mutex_lock(&f.lock); f.fail = false; pthread_mutex_unlock(&f.lock);
    epd_scan_worker_enable(&w, true); image[0] = 3;
    assert(epd_scan_worker_submit(&w, image) == ESP_OK);
    wait_scans(&w, 3); assert(f.seen[1] == 3); finish(&w, &f);
}
static void test_shutdown_joins_callback(void) {
    epd_scan_worker_t w; fixture_t f; begin(&w, &f);
    pthread_t stop; assert(pthread_create(&stop, NULL, stopper, &w) == 0);
    const int64_t deadline = esp_timer_get_time() + 2000000;
    for (;;) {
        xSemaphoreTake(w.lock, portMAX_DELAY); bool stopping = w.stopping; xSemaphoreGive(w.lock);
        if (stopping) break;
        assert(esp_timer_get_time() < deadline); sleep_ms(1);
    }
    uint8_t image[4] = {5};
    assert(epd_scan_worker_submit(&w, image) == ESP_ERR_INVALID_STATE);
    epd_scan_worker_enable(&w, true);
    assert(epd_scan_worker_set_period(&w, 20) == ESP_ERR_INVALID_STATE);
    release(&f); pthread_join(stop, NULL);
    assert(w.task == NULL && w.latest == NULL && w.snapshot == NULL);
    finish(&w, &f);
}
int main(void) {
    test_busy_submission_and_coalescing(); test_disable_discards_inflight_budget();
    test_redrives_between_image_slots(); test_new_image_interrupts_unfinished_redrives();
    test_error_drops_queued_target();
    test_shutdown_joins_callback();
    epd_scan_worker_t w = {0}; fail_create = true;
    assert(epd_scan_worker_init(&w, 4, process, NULL) == ESP_ERR_NO_MEM);
    assert(w.task == NULL && w.latest == NULL && w.snapshot == NULL);
    puts("EPD scan worker concurrency and lifecycle: ok");
}
