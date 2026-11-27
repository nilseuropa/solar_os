#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "esp_err.h"

typedef void *TaskHandle_t;
typedef struct {
    TaskHandle_t task;
    void *mutex, *requested;
    bool stop_requested, task_done, high_refresh_active;
    bool queued_ready, presenting_valid, clear_background_pending;
    uint8_t *queued, *presenting, *mono, *writing;
    size_t source_size;
    char target_name[16];
} solar_os_frame_presenter_t;
#define pdTRUE 1
#define portMAX_DELAY 0
#define SOLAR_OS_TASK_STOP_WAIT_MS 1000U
#define SOLAR_OS_LOGW(...) ((void)0)
static bool joined;
static int restore_result;
static unsigned restores, deletions, frees;
static int xSemaphoreTake(void *mutex, unsigned timeout)
{ (void)timeout; assert(mutex); return pdTRUE; }
static void xSemaphoreGive(void *mutex) { assert(mutex); }
static void vSemaphoreDelete(void *semaphore) { assert(semaphore); deletions++; }
static void solar_os_memory_free(void *data) { if (data) frees++; }
static bool solar_os_task_wait_done(TaskHandle_t task, bool *done, unsigned timeout)
{ assert(task && done && timeout == SOLAR_OS_TASK_STOP_WAIT_MS); return joined; }
static int solar_os_display_set_high_refresh_override(const char *target, bool active, unsigned rate)
{ assert(target && !active && !rate); restores++; return restore_result; }
#include "frame_lifecycle.inc"

int main(void)
{
    uint8_t previous[] = {1, 2, 3}, queued[] = {0, 0, 0};
    solar_os_frame_presenter_t redraw = {
        .task = &redraw, .mutex = &redraw, .requested = &redraw,
        .presenting = previous, .queued = queued, .source_size = sizeof(previous),
    };
    assert(!solar_os_frame_presenter_redraw(NULL));
    assert(!solar_os_frame_presenter_redraw(&redraw)); /* No frame yet. */
    redraw.presenting_valid = true;
    assert(solar_os_frame_presenter_redraw(&redraw));
    assert(redraw.queued_ready);
    assert(!memcmp(previous, queued, sizeof(previous)));
    queued[0] = 9;
    assert(solar_os_frame_presenter_redraw(&redraw) && queued[0] == 9); /* New guest frame wins. */
    redraw.stop_requested = true;
    assert(!solar_os_frame_presenter_redraw(&redraw));
    redraw.stop_requested = false; redraw.task = NULL;
    assert(!solar_os_frame_presenter_redraw(&redraw));
    assert(solar_os_frame_presenter_suspend_checked(NULL) == ESP_OK);
    solar_os_frame_presenter_t presenter = {
        .task = &presenter, .mutex = &presenter, .requested = &presenter,
        .high_refresh_active = true,
    };
    assert(solar_os_frame_presenter_deinit(&presenter) == ESP_ERR_TIMEOUT);
    assert(presenter.task && presenter.stop_requested && presenter.high_refresh_active && !restores && !deletions && !frees);
    joined = true; restore_result = ESP_FAIL;
    assert(solar_os_frame_presenter_deinit(&presenter) == ESP_FAIL);
    assert(!presenter.task && presenter.high_refresh_active && restores == 1 && !deletions && !frees);
    restore_result = ESP_OK;
    assert(solar_os_frame_presenter_deinit(&presenter) == ESP_OK);
    assert(!presenter.task && !presenter.high_refresh_active && restores == 2 && deletions == 2 && frees == 1);
    puts("Presenter suspension joins display work before releasing ownership");
    return 0;
}
