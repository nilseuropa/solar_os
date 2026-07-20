#include "solar_os_jobs.h"

#include <stdio.h>
#include <inttypes.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "jobs/solar_os_job_registry.h"
#include "solar_os_log.h"

typedef struct {
    const solar_os_job_registry_entry_t *entry;
    solar_os_job_state_t state;
    esp_err_t last_error;
    uint32_t tick_count;
    uint32_t last_tick_ms;
    uint32_t generation;
    solar_os_tick_stats_t tick_stats;
    size_t callback_refs;
    bool lifecycle_busy;
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    size_t resource_count;
    solar_os_job_resource_t resources[SOLAR_OS_JOB_RESOURCE_MAX];
} solar_os_job_runtime_t;

static solar_os_job_runtime_t job_runtimes[SOLAR_OS_JOBS_MAX];
static size_t job_runtime_count;
static bool jobs_initialized;
static portMUX_TYPE jobs_lock = portMUX_INITIALIZER_UNLOCKED;

static int job_index_by_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }

    for (size_t i = 0; i < job_runtime_count; i++) {
        if (job_runtimes[i].entry != NULL &&
            job_runtimes[i].entry->name != NULL &&
            strcmp(job_runtimes[i].entry->name, name) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int job_index_by_owner(const char *owner)
{
    if (owner == NULL || owner[0] == '\0') {
        return -1;
    }

    for (size_t i = 0; i < job_runtime_count; i++) {
        const solar_os_job_runtime_t *runtime = &job_runtimes[i];
        if (runtime->entry == NULL || runtime->entry->name == NULL) {
            continue;
        }
        if (runtime->owner[0] != '\0' && strcmp(runtime->owner, owner) == 0) {
            return (int)i;
        }
        if (strcmp(runtime->entry->name, owner) == 0) {
            return (int)i;
        }
        if (strncmp(owner, "job:", 4) == 0 && strcmp(runtime->entry->name, owner + 4) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static solar_os_job_kind_t job_kind_from_runtime(const solar_os_job_runtime_t *runtime)
{
    if (runtime == NULL ||
        runtime->entry == NULL ||
        runtime->entry->job == NULL) {
        return SOLAR_OS_JOB_KIND_BACKGROUND;
    }
    return runtime->entry->job->kind;
}

static void job_clear_resources_by_index(size_t index)
{
    if (index >= job_runtime_count) {
        return;
    }

    solar_os_job_runtime_t *runtime = &job_runtimes[index];
    runtime->resource_count = 0;
    memset(runtime->resources, 0, sizeof(runtime->resources));
}

static uint32_t job_next_generation(solar_os_job_runtime_t *runtime)
{
    runtime->generation++;
    if (runtime->generation == 0) {
        runtime->generation++;
    }
    return runtime->generation;
}

static bool job_status_from_runtime(size_t index, solar_os_job_status_t *status)
{
    if (status == NULL || index >= job_runtime_count || job_runtimes[index].entry == NULL) {
        return false;
    }

    const solar_os_job_runtime_t *runtime = &job_runtimes[index];
    *status = (solar_os_job_status_t){
        .name = runtime->entry->name,
        .summary = runtime->entry->summary,
        .kind = job_kind_from_runtime(runtime),
        .state = runtime->state,
        .last_error = runtime->last_error,
        .tick_count = runtime->tick_count,
        .last_tick_ms = runtime->last_tick_ms,
        .generation = runtime->generation,
        .has_event = runtime->entry->job != NULL && runtime->entry->job->event != NULL,
        .tick_stats = runtime->tick_stats,
        .resource_count = runtime->resource_count,
    };
    strlcpy(status->owner, runtime->owner, sizeof(status->owner));
    memcpy(status->resources,
           runtime->resources,
           runtime->resource_count * sizeof(runtime->resources[0]));
    return true;
}

esp_err_t solar_os_jobs_init(void)
{
    esp_err_t ret = ESP_OK;

    portENTER_CRITICAL(&jobs_lock);
    if (!jobs_initialized) {
        memset(job_runtimes, 0, sizeof(job_runtimes));
        job_runtime_count = solar_os_job_registry_count();
        if (job_runtime_count != SOLAR_OS_JOBS_MAX) {
            ret = ESP_ERR_INVALID_SIZE;
        }

        for (size_t i = 0; ret == ESP_OK && i < job_runtime_count; i++) {
            job_runtimes[i].entry = solar_os_job_registry_get(i);
            job_runtimes[i].state = SOLAR_OS_JOB_STOPPED;
            job_runtimes[i].last_error = ESP_OK;
            if (job_runtimes[i].entry != NULL) {
                (void)solar_os_jobs_owner_name(job_runtimes[i].entry->name,
                                               job_runtimes[i].owner,
                                               sizeof(job_runtimes[i].owner));
            }
        }
        jobs_initialized = ret == ESP_OK;
    }
    portEXIT_CRITICAL(&jobs_lock);
    return ret;
}

size_t solar_os_jobs_count(void)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return 0;
    }
    portENTER_CRITICAL(&jobs_lock);
    const size_t count = job_runtime_count;
    portEXIT_CRITICAL(&jobs_lock);
    return count;
}

bool solar_os_jobs_get(size_t index, solar_os_job_status_t *status)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return false;
    }
    portENTER_CRITICAL(&jobs_lock);
    const bool found = job_status_from_runtime(index, status);
    portEXIT_CRITICAL(&jobs_lock);
    return found;
}

bool solar_os_jobs_get_by_name(const char *name, solar_os_job_status_t *status)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return false;
    }

    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    const bool found = index >= 0 && job_status_from_runtime((size_t)index, status);
    portEXIT_CRITICAL(&jobs_lock);
    return found;
}

bool solar_os_jobs_get_by_owner(const char *owner, solar_os_job_status_t *status)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return false;
    }

    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_owner(owner);
    const bool found = index >= 0 && job_status_from_runtime((size_t)index, status);
    portEXIT_CRITICAL(&jobs_lock);
    return found;
}

esp_err_t solar_os_jobs_start(solar_os_context_t *ctx, const char *name, int argc, char **argv)
{
    esp_err_t ret = solar_os_jobs_init();
    if (ret != ESP_OK) {
        return ret;
    }

    if (argc < 0 || argc > SOLAR_OS_APP_ARG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < argc; i++) {
        if (argv == NULL || argv[i] == NULL || strlen(argv[i]) >= SOLAR_OS_APP_ARG_LEN) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    void (*stop)(solar_os_context_t *ctx) = NULL;
    esp_err_t (*start)(solar_os_context_t *ctx, int argc, char **argv) = NULL;
    uint32_t generation = 0;
    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index < 0) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_job_runtime_t *runtime = &job_runtimes[index];
    if (runtime->lifecycle_busy || runtime->entry == NULL || runtime->entry->job == NULL) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->lifecycle_busy = true;
    stop = runtime->state == SOLAR_OS_JOB_RUNNING ? runtime->entry->job->stop : NULL;
    start = runtime->entry->job->start;
    generation = job_next_generation(runtime);
    runtime->state = SOLAR_OS_JOB_STOPPED;
    job_clear_resources_by_index((size_t)index);
    portEXIT_CRITICAL(&jobs_lock);

    for (;;) {
        portENTER_CRITICAL(&jobs_lock);
        const size_t callback_refs = runtime->callback_refs;
        portEXIT_CRITICAL(&jobs_lock);
        if (callback_refs == 0) {
            break;
        }
        vTaskDelay(1);
    }
    if (stop != NULL) {
        stop(ctx);
    }

    ret = ESP_OK;
    if (start != NULL) {
        ret = start(ctx, argc, argv);
    }

    portENTER_CRITICAL(&jobs_lock);
    if (runtime->generation == generation && runtime->lifecycle_busy) {
        runtime->last_error = ret;
        runtime->tick_count = 0;
        runtime->last_tick_ms = 0;
        solar_os_tick_stats_reset(&runtime->tick_stats);
        runtime->state = ret == ESP_OK ? SOLAR_OS_JOB_RUNNING : SOLAR_OS_JOB_FAILED;
        runtime->lifecycle_busy = false;
        if (ret != ESP_OK) {
            job_clear_resources_by_index((size_t)index);
        }
    }
    portEXIT_CRITICAL(&jobs_lock);
    return ret;
}

esp_err_t solar_os_jobs_stop(solar_os_context_t *ctx, const char *name)
{
    esp_err_t ret = solar_os_jobs_init();
    if (ret != ESP_OK) {
        return ret;
    }

    void (*stop)(solar_os_context_t *ctx) = NULL;
    uint32_t generation = 0;
    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index < 0) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_job_runtime_t *runtime = &job_runtimes[index];
    if (runtime->lifecycle_busy) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (runtime->state != SOLAR_OS_JOB_RUNNING) {
        runtime->state = SOLAR_OS_JOB_STOPPED;
        job_clear_resources_by_index((size_t)index);
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_OK;
    }
    runtime->lifecycle_busy = true;
    stop = runtime->entry != NULL && runtime->entry->job != NULL ?
        runtime->entry->job->stop : NULL;
    generation = job_next_generation(runtime);
    runtime->state = SOLAR_OS_JOB_STOPPED;
    job_clear_resources_by_index((size_t)index);
    portEXIT_CRITICAL(&jobs_lock);

    for (;;) {
        portENTER_CRITICAL(&jobs_lock);
        const size_t callback_refs = runtime->callback_refs;
        portEXIT_CRITICAL(&jobs_lock);
        if (callback_refs == 0) {
            break;
        }
        vTaskDelay(1);
    }
    if (stop != NULL) {
        stop(ctx);
    }

    portENTER_CRITICAL(&jobs_lock);
    if (runtime->generation == generation && runtime->lifecycle_busy) {
        runtime->last_error = ESP_OK;
        runtime->lifecycle_busy = false;
    }
    portEXIT_CRITICAL(&jobs_lock);
    return ESP_OK;
}

esp_err_t solar_os_jobs_get_generation(const char *name, uint32_t *generation)
{
    if (generation == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = solar_os_jobs_init();
    if (ret != ESP_OK) {
        return ret;
    }
    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index < 0) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NOT_FOUND;
    }
    *generation = job_runtimes[index].generation;
    portEXIT_CRITICAL(&jobs_lock);
    return ESP_OK;
}

esp_err_t solar_os_jobs_mark_stopped(const char *name,
                                     uint32_t generation,
                                     esp_err_t last_error)
{
    esp_err_t ret = solar_os_jobs_init();
    if (ret != ESP_OK) {
        return ret;
    }
    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index < 0) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NOT_FOUND;
    }
    solar_os_job_runtime_t *runtime = &job_runtimes[index];
    if (generation == 0 || runtime->generation != generation || runtime->lifecycle_busy) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_INVALID_STATE;
    }
    runtime->state = SOLAR_OS_JOB_STOPPED;
    runtime->last_error = last_error;
    job_clear_resources_by_index((size_t)index);
    portEXIT_CRITICAL(&jobs_lock);
    return ESP_OK;
}

void solar_os_jobs_tick(solar_os_context_t *ctx, uint32_t now_ms)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return;
    }

    const solar_os_event_t event = {
        .type = SOLAR_OS_EVENT_TICK,
        .data.tick_ms = now_ms,
    };

    const size_t job_count = solar_os_jobs_count();
    for (size_t i = 0; i < job_count; i++) {
        bool (*callback)(solar_os_context_t *ctx, const solar_os_event_t *event) = NULL;
        uint32_t generation = 0;
        int64_t started_us = 0;
        portENTER_CRITICAL(&jobs_lock);
        solar_os_job_runtime_t *runtime = &job_runtimes[i];
        if (runtime->state == SOLAR_OS_JOB_RUNNING &&
            !runtime->lifecycle_busy &&
            runtime->entry != NULL &&
            runtime->entry->job != NULL &&
            runtime->entry->job->event != NULL &&
            solar_os_tick_due(&runtime->tick_stats,
                              runtime->entry->job->tick_interval_ms,
                              runtime->entry->job->tick_deadline_ms,
                              SOLAR_OS_TICK_INTERVAL_DEFAULT_MS,
                              SOLAR_OS_TICK_DEADLINE_DEFAULT_MS,
                              now_ms)) {
            callback = runtime->entry->job->event;
            generation = runtime->generation;
            runtime->callback_refs++;
        }
        portEXIT_CRITICAL(&jobs_lock);
        if (callback == NULL) {
            continue;
        }

        started_us = solar_os_tick_begin();
        (void)callback(ctx, &event);
        bool deadline_missed = false;
        solar_os_tick_stats_t tick_stats = {0};
        portENTER_CRITICAL(&jobs_lock);
        if (runtime->callback_refs > 0) {
            runtime->callback_refs--;
        }
        if (runtime->generation == generation &&
            runtime->state == SOLAR_OS_JOB_RUNNING) {
            deadline_missed = solar_os_tick_end(&runtime->tick_stats, started_us);
            runtime->tick_count = runtime->tick_stats.dispatch_count;
            runtime->last_tick_ms = now_ms;
            tick_stats = runtime->tick_stats;
        }
        portEXIT_CRITICAL(&jobs_lock);
        if (deadline_missed && solar_os_tick_should_log_miss(&tick_stats)) {
            SOLAR_OS_LOGW("solar_os_jobs",
                          "tick miss: %s %" PRIu32 "us>%" PRIu32 "ms n=%" PRIu32,
                          runtime->entry->name,
                          tick_stats.last_duration_us,
                          tick_stats.deadline_ms,
                          tick_stats.deadline_miss_count);
        }
    }
}

esp_err_t solar_os_jobs_owner_name(const char *name, char *owner, size_t owner_len)
{
    if (name == NULL || name[0] == '\0' || owner == NULL || owner_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const int written = snprintf(owner, owner_len, "job:%s", name);
    if (written < 0 || (size_t)written >= owner_len) {
        owner[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_jobs_note_resource(const char *name,
                                      solar_os_job_resource_type_t type,
                                      const char *resource,
                                      const char *detail)
{
    esp_err_t ret = solar_os_jobs_init();
    if (ret != ESP_OK) {
        return ret;
    }
    if (type == SOLAR_OS_JOB_RESOURCE_NONE ||
        resource == NULL ||
        resource[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index < 0) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NOT_FOUND;
    }

    solar_os_job_runtime_t *runtime = &job_runtimes[index];
    for (size_t i = 0; i < runtime->resource_count; i++) {
        solar_os_job_resource_t *existing = &runtime->resources[i];
        if (existing->type == type && strcmp(existing->name, resource) == 0) {
            strlcpy(existing->detail,
                    detail != NULL ? detail : "",
                    sizeof(existing->detail));
            portEXIT_CRITICAL(&jobs_lock);
            return ESP_OK;
        }
    }

    if (runtime->resource_count >= SOLAR_OS_JOB_RESOURCE_MAX) {
        portEXIT_CRITICAL(&jobs_lock);
        return ESP_ERR_NO_MEM;
    }

    solar_os_job_resource_t *slot = &runtime->resources[runtime->resource_count++];
    memset(slot, 0, sizeof(*slot));
    slot->type = type;
    strlcpy(slot->name, resource, sizeof(slot->name));
    strlcpy(slot->detail, detail != NULL ? detail : "", sizeof(slot->detail));
    portEXIT_CRITICAL(&jobs_lock);
    return ESP_OK;
}

void solar_os_jobs_clear_resources(const char *name)
{
    if (solar_os_jobs_init() != ESP_OK) {
        return;
    }

    portENTER_CRITICAL(&jobs_lock);
    const int index = job_index_by_name(name);
    if (index >= 0) {
        job_clear_resources_by_index((size_t)index);
    }
    portEXIT_CRITICAL(&jobs_lock);
}

esp_err_t solar_os_jobs_claim_port(const char *name,
                                   const char *port_name,
                                   solar_os_port_handle_t *handle)
{
    char owner[SOLAR_OS_JOB_OWNER_MAX];
    esp_err_t err = solar_os_jobs_owner_name(name, owner, sizeof(owner));
    if (err != ESP_OK) {
        return err;
    }

    err = solar_os_port_claim(port_name, owner, handle);
    if (err != ESP_OK) {
        return err;
    }

    (void)solar_os_jobs_note_resource(name, SOLAR_OS_JOB_RESOURCE_PORT, port_name, "rw");
    return ESP_OK;
}

const char *solar_os_job_state_name(solar_os_job_state_t state)
{
    switch (state) {
    case SOLAR_OS_JOB_STOPPED:
        return "stopped";
    case SOLAR_OS_JOB_RUNNING:
        return "running";
    case SOLAR_OS_JOB_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char *solar_os_job_kind_name(solar_os_job_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_JOB_KIND_BACKGROUND:
        return "background";
    case SOLAR_OS_JOB_KIND_INTERACTIVE:
        return "interactive";
    default:
        return "unknown";
    }
}

const char *solar_os_job_resource_type_name(solar_os_job_resource_type_t type)
{
    switch (type) {
    case SOLAR_OS_JOB_RESOURCE_PORT:
        return "port";
    case SOLAR_OS_JOB_RESOURCE_FILE:
        return "file";
    case SOLAR_OS_JOB_RESOURCE_NET:
        return "net";
    case SOLAR_OS_JOB_RESOURCE_STREAM:
        return "stream";
    case SOLAR_OS_JOB_RESOURCE_CUSTOM:
        return "custom";
    case SOLAR_OS_JOB_RESOURCE_NONE:
    default:
        return "none";
    }
}
