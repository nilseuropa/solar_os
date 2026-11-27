#include "solar_os_email_sync_job.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_email.h"
#include "solar_os_jobs.h"
#include "solar_os_task.h"

#define EMAIL_SYNC_DEFAULT_INTERVAL_SEC 300U
#define EMAIL_SYNC_MIN_INTERVAL_SEC 30U
#define EMAIL_SYNC_MAX_INTERVAL_SEC 86400U
#define EMAIL_SYNC_TASK_STACK 12288U

static struct {
    bool running;
    bool once;
    bool started;
    bool rescan;
    atomic_bool sync_in_progress;
    atomic_bool stop_requested;
    uint32_t interval_ms;
    uint32_t next_sync_ms;
    uint32_t generation;
    esp_err_t last_error;
} email_sync;

static bool email_sync_parse_interval(const char *text, uint32_t *seconds)
{
    if (text == NULL || seconds == NULL) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < EMAIL_SYNC_MIN_INTERVAL_SEC || value > EMAIL_SYNC_MAX_INTERVAL_SEC) {
        return false;
    }
    *seconds = (uint32_t)value;
    return true;
}

static bool email_sync_parse_args(int argc, char **argv, uint32_t *seconds, bool *once)
{
    *seconds = EMAIL_SYNC_DEFAULT_INTERVAL_SEC;
    *once = false;
    int first = 0;
    if (argc > 0 && argv != NULL && argv[0] != NULL &&
        strcmp(argv[0], solar_os_email_sync_job.name) == 0) {
        first = 1;
    }
    bool interval_set = false;
    for (int i = first; i < argc; i++) {
        if (strcmp(argv[i], "once") == 0) {
            if (*once) {
                return false;
            }
            *once = true;
        } else if (!interval_set && email_sync_parse_interval(argv[i], seconds)) {
            interval_set = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool email_sync_stop_requested(void *user)
{
    (void)user;
    return atomic_load(&email_sync.stop_requested);
}

static void email_sync_task(void *arg)
{
    (void)arg;
    email_sync.last_error = solar_os_email_sync_fetch(
        email_sync.rescan, email_sync_stop_requested, NULL);
    if (email_sync.last_error != ESP_ERR_NOT_FINISHED) email_sync.rescan = false;
    atomic_store(&email_sync.sync_in_progress, false);
    solar_os_task_delete(NULL);
}

static esp_err_t email_sync_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    if (atomic_load(&email_sync.sync_in_progress)) return ESP_ERR_INVALID_STATE;
    uint32_t interval_sec;
    bool once;
    if (!email_sync_parse_args(argc, argv, &interval_sec, &once)) return ESP_ERR_INVALID_ARG;

    solar_os_email_status_t status;
    esp_err_t error = solar_os_email_get_status(&status);
    if (error != ESP_OK) return error;
    if (!status.configured) return ESP_ERR_INVALID_STATE;
    error = solar_os_jobs_get_generation(solar_os_email_sync_job.name, &email_sync.generation);
    if (error != ESP_OK) return error;
    email_sync.running = true;
    email_sync.once = once;
    email_sync.started = false;
    email_sync.rescan = true;
    email_sync.next_sync_ms = 0;
    email_sync.interval_ms = interval_sec * 1000U;
    atomic_store(&email_sync.stop_requested, false);
    (void)solar_os_jobs_note_resource(solar_os_email_sync_job.name,
        SOLAR_OS_JOB_RESOURCE_NET, status.url, "imaps");
    return ESP_OK;
}

static void email_sync_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    email_sync.running = false;
    atomic_store(&email_sync.stop_requested, true);
}

static bool email_sync_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (!event || event->type != SOLAR_OS_EVENT_TICK || !email_sync.running ||
        atomic_load(&email_sync.sync_in_progress)) return false;

    if (email_sync.once && email_sync.started && email_sync.last_error != ESP_ERR_NOT_FINISHED) {
        email_sync.running = false;
        (void)solar_os_jobs_mark_stopped(solar_os_email_sync_job.name,
            email_sync.generation, email_sync.last_error);
        return true;
    }
    const uint32_t now = event->data.tick_ms;
    if (email_sync.next_sync_ms && (int32_t)(now - email_sync.next_sync_ms) < 0) return false;
    email_sync.next_sync_ms = now + (email_sync.once ? 100U : email_sync.interval_ms);
    email_sync.started = true;
    atomic_store(&email_sync.sync_in_progress, true);
    if (solar_os_task_create_pinned(email_sync_task, "email_sync", EMAIL_SYNC_TASK_STACK,
            NULL, tskIDLE_PRIORITY + 2, NULL, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        email_sync.last_error = ESP_ERR_NO_MEM;
        atomic_store(&email_sync.sync_in_progress, false);
    }
    return true;
}

const solar_os_job_t solar_os_email_sync_job = {
    .name = "email-sync",
    .summary = "periodic IMAP email synchronization",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND,
    .start = email_sync_start,
    .stop = email_sync_stop,
    .event = email_sync_event,
    .worker_stack_bytes = EMAIL_SYNC_TASK_STACK,
    .tick_interval_ms = 100U,
    .tick_deadline_ms = 10U,
};
