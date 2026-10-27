#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SOLAR_OS_FTP_NAME_MAX 64
#define SOLAR_OS_STORAGE_PATH_MAX 128
#define SOLAR_OS_NET_HOST_MAX 64
#define SOLAR_OS_FTP_REPLY_MAX 128
#define FTP_APP_INPUT_MAX 80
#define FTP_APP_MESSAGE_MAX 128
#define FTP_APP_PROTOCOL "FTP"
#define ESP_OK 0
#define ESP_ERR_INVALID_CRC 10
#define pdTRUE 1
#define pdFALSE 0
typedef int esp_err_t;
typedef int solar_os_tui_t;
typedef int solar_os_tui_input_state_t;
typedef int solar_os_text_search_state_t;
typedef int solar_os_context_t;
typedef void *QueueHandle_t;
typedef void *TaskHandle_t;

/* TYPES */

static ftp_app_state_t ftp_app;
static ftp_app_request_t submitted[16];
static size_t submitted_count;
static ftp_app_result_t pending_result;
static bool result_ready;
static bool refuse_submit;
static bool refuse_alloc;
static const char *esp_err_to_name(esp_err_t error) { (void)error; return "failed"; }
static size_t strlcpy(char *out, const char *text, size_t size)
{
    size_t len = strlen(text);
    if (size) {
        size_t copied = len < size - 1 ? len : size - 1;
        memcpy(out, text, copied);
        out[copied] = '\0';
    }
    return len;
}
static void *ftp_app_realloc(void *pointer, size_t size)
{
    return refuse_alloc ? NULL : realloc(pointer, size);
}
static void solar_os_memory_free(void *pointer) { free(pointer); }
static void ftp_app_set_message(const char *message)
{
    strlcpy(ftp_app.message, message, sizeof(ftp_app.message));
}
static int xQueueSend(QueueHandle_t queue, const ftp_app_request_t *request, int ticks)
{
    (void)queue; (void)ticks;
    if (refuse_submit) return pdFALSE;
    assert(submitted_count < sizeof(submitted) / sizeof(submitted[0]));
    submitted[submitted_count++] = *request;
    return pdTRUE;
}
static int xQueueReceive(QueueHandle_t queue, void *result, int ticks)
{
    (void)ticks;
    assert(queue == ftp_app.results);
    if (!result_ready) return pdFALSE;
    *(ftp_app_result_t *)result = pending_result;
    result_ready = false;
    return pdTRUE;
}
static void ftp_app_render(void) {}
static void ftp_app_begin_connection(void) {}
static void ftp_app_view_local(solar_os_context_t *ctx, const char *path)
{
    (void)ctx; (void)path;
}
static esp_err_t ftp_app_refresh_local(void) { return ESP_OK; }

/* SOURCE */

static ftp_app_entry_t *entries(size_t *count)
{
    ftp_app_entry_t initial[] = {
        {.name = "..", .is_directory = true, .parent = true},
        {.name = "docs", .is_directory = true},
        {.name = "first", .size = 17},
        {.name = "second", .size = 23},
        {.name = ".secret", .size = 2},
    };
    *count = sizeof(initial) / sizeof(initial[0]);
    ftp_app_entry_t *copy = malloc(sizeof(initial));
    assert(copy);
    memcpy(copy, initial, sizeof(initial));
    return copy;
}

static void reset(void)
{
    ftp_app_batch_clear();
    for (unsigned i = 0; i < 2; i++) free(ftp_app.panes[i].entries);
    memset(&ftp_app, 0, sizeof(ftp_app));
    ftp_app.show_hidden = true;
    ftp_app.requests = (void *)1;
    ftp_app.results = (void *)2;
    strlcpy(ftp_app.panes[0].path, "/local", sizeof(ftp_app.panes[0].path));
    strlcpy(ftp_app.panes[1].path, "/remote", sizeof(ftp_app.panes[1].path));
    for (unsigned i = 0; i < 2; i++) {
        ftp_app_pane_t *pane = &ftp_app.panes[i];
        pane->entries = entries(&pane->count);
        pane->listed_count = pane->count;
    }
    result_ready = false;
    refuse_submit = false;
    refuse_alloc = false;
    submitted_count = 0;
}

static void complete(esp_err_t error, bool operation_done)
{
    pending_result = (ftp_app_result_t){
        .kind = submitted[submitted_count - 1].kind,
        .error = error,
        .operation_done = operation_done,
    };
    if (error == ESP_OK) {
        pending_result.remote_entries = entries(&pending_result.remote_count);
        strlcpy(pending_result.remote_path, "/remote", sizeof(pending_result.remote_path));
    }
    result_ready = true;
    ftp_app_poll(NULL);
}

static void mark_pair(void)
{
    ftp_app_pane_t *pane = &ftp_app.panes[ftp_app.active];
    pane->cursor = 2;
    ftp_app_toggle_selection(pane);
    ftp_app_toggle_selection(pane);
    assert(ftp_app_selection_count(pane) == 2);
    assert(pane->cursor == 4);
}

int main(void)
{
    reset();
    ftp_app_pane_t *pane = &ftp_app.panes[0];
    ftp_app_toggle_selection(pane);
    assert(!pane->entries[0].selected && pane->cursor == 0);
    pane->cursor = 4;
    ftp_app_toggle_selection(pane);
    assert(pane->cursor == 4 && pane->entries[4].selected);
    ftp_app_toggle_selection(pane);
    assert(!pane->entries[4].selected);
    mark_pair();
    assert(!strcmp(ftp_app.message, "2 selected"));
    char path[SOLAR_OS_STORAGE_PATH_MAX];
    assert(ftp_app_selected_path(path, sizeof(path)) && !strcmp(path, "/local/.secret"));
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    assert(submitted_count == 1 && ftp_app.busy);
    assert(!strcmp(submitted[0].source, "/local/first"));
    assert(!strcmp(submitted[0].destination, "/remote/first"));
    assert(submitted[0].size == 17 && !submitted[0].remote_source);
    /* A reordered listing/cursor must not change the snapshot's next item. */
    ftp_app_entry_t swap = pane->entries[2];
    pane->entries[2] = pane->entries[3];
    pane->entries[3] = swap;
    pane->cursor = 0;
    complete(ESP_OK, true);
    assert(submitted_count == 2 && ftp_app.busy);
    assert(!strcmp(submitted[1].source, "/local/second"));
    assert(ftp_app_selection_count(pane) == 1);
    complete(ESP_OK, true);
    assert(submitted_count == 2 && !ftp_app.busy && !ftp_app.batch_requests);
    assert(ftp_app_selection_count(pane) == 0);

    reset();
    ftp_app.active = 1;
    mark_pair();
    ftp_app_begin_operation(FTP_APP_WORK_MOVE);
    assert(submitted[0].remote_source && submitted[0].kind == FTP_APP_WORK_MOVE);
    assert(!strcmp(submitted[0].source, "/remote/first"));
    assert(!strcmp(submitted[0].destination, "/local/first"));
    complete(ESP_OK, true);
    /* The remote refresh preserves only the still-pending mark. */
    assert(ftp_app_selection_count(&ftp_app.panes[1]) == 1);
    complete(-1, false);
    assert(submitted_count == 2 && !ftp_app.batch_requests && !ftp_app.busy);
    assert(ftp_app_selection_count(&ftp_app.panes[1]) == 1);
    assert(!strcmp(ftp_app.message, "failed"));

    reset();
    mark_pair();
    ftp_app_begin_operation(FTP_APP_WORK_DELETE);
    assert(submitted[0].kind == FTP_APP_WORK_DELETE && submitted[0].destination[0] == '\0');
    complete(-1, false);
    assert(submitted_count == 1 && !ftp_app.batch_requests);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 2);

    reset();
    mark_pair();
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    /* An operation can finish before its remote listing fails. */
    complete(-1, true);
    assert(submitted_count == 1 && !ftp_app.batch_requests);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 1);

    reset();
    ftp_app.panes[0].cursor = 1;
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    assert(submitted_count == 1 && submitted[0].is_directory);
    complete(ESP_OK, true);
    assert(!ftp_app.batch_requests);

    reset();
    ftp_app_begin_operation(FTP_APP_WORK_DELETE);
    assert(submitted_count == 0 && !ftp_app.batch_requests);
    mark_pair();
    refuse_alloc = true;
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    assert(submitted_count == 0 && !ftp_app.batch_requests);
    refuse_alloc = false;
    refuse_submit = true;
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    assert(submitted_count == 0 && !ftp_app.batch_requests);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 2);

    reset();
    mark_pair();
    memset(ftp_app.panes[1].path, 'x', sizeof(ftp_app.panes[1].path) - 1);
    ftp_app_begin_operation(FTP_APP_WORK_COPY);
    assert(submitted_count == 0 && !ftp_app.batch_requests);

    reset();
    ftp_app.panes[0].entries[4].selected = true;
    ftp_app.show_hidden = false;
    ftp_app_filter_hidden(&ftp_app.panes[0]);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 0);
    ftp_app.show_hidden = true;
    ftp_app_filter_hidden(&ftp_app.panes[0]);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 0);

    /* Directory changes never carry marks into similarly named entries. */
    ftp_app_pane_t previous = ftp_app.panes[0];
    previous.entries[2].selected = true;
    ftp_app.panes[0].entries = entries(&ftp_app.panes[0].count);
    strlcpy(ftp_app.panes[0].path, "/other", sizeof(ftp_app.panes[0].path));
    ftp_app_preserve_selection(&ftp_app.panes[0], &previous);
    assert(ftp_app_selection_count(&ftp_app.panes[0]) == 0);
    free(previous.entries);
    reset();
    for (unsigned i = 0; i < 2; i++) free(ftp_app.panes[i].entries);
    return 0;
}
