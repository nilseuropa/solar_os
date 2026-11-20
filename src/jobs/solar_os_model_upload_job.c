#include "solar_os_model_upload_job.h"
#include "solar_os_jobs.h"
#include "solar_os_memory.h"
#include "solar_os_processor.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

#define UPLOAD_STACK 16384U
typedef struct {
    char kind[12], source[32], bundle[160], stage[32];
    uint32_t slot, flags, generation, bytes, total;
    esp_err_t error;
    TaskHandle_t task;
    volatile bool cancel, done;
    bool pending;
} upload_state_t;
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("model upload job state pointer and progress handoff lock")
static upload_state_t *state;
static portMUX_TYPE upload_lock = portMUX_INITIALIZER_UNLOCKED;
static bool cancel(void *user) { return ((upload_state_t *)user)->cancel; }
static void progress(const char *stage, uint32_t bytes, uint32_t total, void *user)
{
    upload_state_t *s = user;
    portENTER_CRITICAL(&upload_lock);
    strlcpy(s->stage, stage, sizeof(s->stage)); s->bytes = bytes; s->total = total;
    portEXIT_CRITICAL(&upload_lock);
}
static void worker(void *user)
{
    upload_state_t *s = user; char *json = NULL;
    esp_err_t error = solar_os_processor_upload(s->kind, s->source, s->bundle, s->slot,
        s->flags, cancel, progress, s, &json);
    solar_os_memory_free(json);
    portENTER_CRITICAL(&upload_lock); s->error = error; s->done = true; portEXIT_CRITICAL(&upload_lock);
    solar_os_task_delete_internal(NULL);
}
static esp_err_t start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;
    int first = argc && argv && argv[0] && !strcmp(argv[0], "model-upload") ? 1 : 0;
    if (!argv || argc < first+4 || argc > first+5 || !solar_os_processor_kind_valid(argv[first]) ||
        !argv[first+1] || !*argv[first+1] || strlen(argv[first+1]) >= 32 || !argv[first+2] ||
        *argv[first+2] != '/' || strlen(argv[first+2]) >= 160 || !argv[first+3] || !*argv[first+3]) return ESP_ERR_INVALID_ARG;
    uint64_t slot = 0;
    for (const char *p = argv[first+3]; *p; ++p) {
        if (*p < '0' || *p > '9' || slot > UINT32_MAX/10U) return ESP_ERR_INVALID_ARG;
        slot = slot*10 + *p-'0'; if (slot > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    }
    if (!slot || (argc == first+5 && strcmp(argv[first+4], "--replace"))) return ESP_ERR_INVALID_ARG;
    if (!state) {
        state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "model.upload");
        if (!state) return ESP_ERR_NO_MEM;
        state->done = true;
    }
    if (!state->done || state->pending) return ESP_ERR_INVALID_STATE;
    esp_err_t error = solar_os_jobs_get_generation("model-upload", &state->generation);
    if (error) return error;
    portENTER_CRITICAL(&upload_lock);
    strlcpy(state->kind, argv[first], sizeof(state->kind));
    strlcpy(state->source, argv[first+1], sizeof(state->source));
    strlcpy(state->bundle, argv[first+2], sizeof(state->bundle));
    state->slot = slot; state->flags = argc == first+5 ? SOLAR_OS_PROCESSOR_UPLOAD_REPLACE : 0;
    state->cancel = false; state->done = false; state->pending = true; state->error = ESP_OK;
    portEXIT_CRITICAL(&upload_lock);
    progress("verify bundle", 0, 0, state);
    (void)solar_os_jobs_note_resource("model-upload", SOLAR_OS_JOB_RESOURCE_FILE, state->bundle, "model bundle");
    (void)solar_os_jobs_note_resource("model-upload", SOLAR_OS_JOB_RESOURCE_STREAM, state->source, "exclusive camera");
    if (solar_os_task_create_pinned_internal(worker, "model-upload", UPLOAD_STACK, state,
        tskIDLE_PRIORITY+1, &state->task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_BACKGROUND) != pdPASS) {
        state->done = true; state->pending = false; state->error = ESP_ERR_NO_MEM; return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static void stop(solar_os_context_t *ctx)
{
    (void)ctx;
    if (!state) return;
    state->cancel = true;
    (void)solar_os_task_wait_done(state->task, &state->done, SOLAR_OS_TASK_STOP_WAIT_MS);
    if (state->done) { state->pending = false; state->task = NULL; }
}
static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;
    if (event && event->type == SOLAR_OS_EVENT_TICK && state && state->pending && state->done) {
        state->pending = false; state->task = NULL;
        (void)solar_os_jobs_mark_stopped("model-upload", state->generation, state->error);
    }
    return false;
}
static void detail(solar_os_context_t *ctx)
{
    if (!state) return;
    char stage[32], source[32], kind[12], bundle[160]; uint32_t bytes, total, slot; bool done; esp_err_t error;
    portENTER_CRITICAL(&upload_lock);
    memcpy(stage, state->stage, sizeof(stage)); memcpy(source, state->source, sizeof(source));
    memcpy(kind, state->kind, sizeof(kind)); memcpy(bundle, state->bundle, sizeof(bundle));
    bytes = state->bytes; total = state->total; slot = state->slot; done = state->done; error = state->error;
    portEXIT_CRITICAL(&upload_lock);
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    solar_os_shell_io_printf(io, "  processor=%s source=%s slot=%lu stage=%s bytes=%lu/%lu done=%s error=%s\n",
        kind, source, (unsigned long)slot, stage, (unsigned long)bytes, (unsigned long)total, done ? "true" : "false", esp_err_to_name(error));
    solar_os_shell_io_printf(io, "  bundle=%s\n", bundle);
}
const solar_os_job_t solar_os_model_upload_job = {
    .name = "model-upload", .summary = "program a model through an optional processor provider",
    .kind = SOLAR_OS_JOB_KIND_BACKGROUND, .start = start, .stop = stop, .event = event, .detail = detail,
    .worker_stack_bytes = UPLOAD_STACK, .tick_interval_ms = 100, .tick_deadline_ms = 10,
};
