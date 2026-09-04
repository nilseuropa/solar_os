#include "solar_os_shell_commands.h"

#include <stdio.h>
#include <string.h>

#include "solar_os_shell_common.h"

#if SOLAR_OS_PACKAGE_APP_DOCS
#include "solar_os_docs_app.h"
#endif
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_docs.h"
#include "solar_os_task.h"
#endif
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_APP_LESS
#include "solar_os_less.h"
#endif
#include "solar_os_manual.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"

#define MAN_QUERY_MAX 160U
#define MAN_SEARCH_MAX 12U
#define DOCS_UPDATE_TASK_STACK 16384U
#define DOCS_UPDATE_WAIT_MS 100U
#define DOCS_PROGRESS_BAR_WIDTH 20U

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(DOCS_UPDATE_TASK_STACK);

static void docs_warn_if_outdated(solar_os_shell_io_t *io)
{
    solar_os_docs_status_t status;
    if (solar_os_docs_get_status(&status) == ESP_OK &&
        status.available &&
        strcmp(status.manual_version, SOLAR_OS_VERSION) != 0) {
        solar_os_shell_io_printf(
            io,
            "Warning: downloaded Help is for SolarOS %s; running %s. "
            "It may be outdated. Run 'help update'.\n",
            status.manual_version,
            SOLAR_OS_VERSION);
    }
}
#endif

static void man_usage(solar_os_shell_io_t *io)
{
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  man TOPIC");
    solar_os_shell_io_writeln(io, "  man -k QUERY...");
    solar_os_shell_io_writeln(io, "  man --list");
}

static bool man_join_args(int argc,
                          char **argv,
                          int first,
                          char *buffer,
                          size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0U || first >= argc) {
        return false;
    }
    buffer[0] = '\0';
    size_t used = 0U;
    for (int i = first; i < argc; i++) {
        const char *arg = argv[i];
        const size_t arg_len = arg != NULL ? strlen(arg) : 0U;
        const size_t separator = used > 0U ? 1U : 0U;
        if (arg_len == 0U || used + separator + arg_len >= buffer_len) {
            return false;
        }
        if (separator != 0U) {
            buffer[used++] = ' ';
        }
        memcpy(buffer + used, arg, arg_len);
        used += arg_len;
        buffer[used] = '\0';
    }
    return used > 0U;
}

static void man_list(solar_os_shell_io_t *io)
{
    for (size_t i = 0U; i < solar_os_manual_count(); i++) {
        const solar_os_manual_page_t *page = solar_os_manual_get(i);
        if (page != NULL) {
            solar_os_shell_io_printf(io,
                                     "%-18s %-8s %s\n",
                                     page->id,
                                     page->section,
                                     page->summary);
        }
    }
}

static size_t man_search(solar_os_shell_io_t *io, const char *query)
{
    const solar_os_manual_page_t *matches[MAN_SEARCH_MAX] = {0};
    const size_t count =
        solar_os_manual_search(query, matches, MAN_SEARCH_MAX);
    if (count == 0U) {
        solar_os_shell_io_printf(io, "man: no entries for %s\n", query);
        return 0U;
    }
    for (size_t i = 0U; i < count; i++) {
        solar_os_shell_io_printf(io,
                                 "%-18s - %s\n",
                                 matches[i]->id,
                                 matches[i]->summary);
    }
    return count;
}

static void man_print_page(solar_os_shell_io_t *io,
                           const solar_os_manual_page_t *page)
{
    const char *body = NULL;
    size_t len = 0U;
    bool owned = false;
    if (page == NULL ||
        solar_os_manual_load_body(page, &body, &len, &owned) != ESP_OK) {
        return;
    }
    solar_os_shell_io_write(io, body);
    if (len == 0U || body[len - 1U] != '\n') {
        solar_os_shell_io_newline(io);
    }
    solar_os_manual_release_text(body, owned);
}

static bool man_launch_pager(solar_os_context_t *ctx,
                             solar_os_shell_io_t *io,
                             const solar_os_manual_page_t *page)
{
#if SOLAR_OS_PACKAGE_APP_LESS
    char source[SOLAR_OS_APP_ARG_LEN];
    const int written = snprintf(source, sizeof(source), "man:%s", page->id);
    if (written < 0 || (size_t)written >= sizeof(source)) {
        return false;
    }
    char *launch_argv[] = {"less", source};
    const esp_err_t err =
        solar_os_context_request_launch(ctx, &solar_os_less_app, 2, launch_argv);
    if (err == ESP_OK) {
        solar_os_shell_session_prepare_foreground_launch(ctx, false);
        return true;
    }
    solar_os_shell_io_printf(io,
                             "man: pager unavailable: %s\n",
                             solar_os_shell_error_text(err));
#else
    (void)ctx;
    (void)io;
    (void)page;
#endif
    return false;
}

void solar_os_shell_cmd_man(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
    if (argc == 2 && strcmp(argv[1], "--list") == 0) {
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
        docs_warn_if_outdated(io);
#endif
        man_list(io);
        return;
    }
    if (argc == 2 && (strcmp(argv[1], "-k") == 0 || strcmp(argv[1], "--apropos") == 0)) {
        solar_os_shell_diag_missing(io, "man -k", "search query", "man -k QUERY");
        return;
    }
    if (argc >= 3 && (strcmp(argv[1], "-k") == 0 ||
                      strcmp(argv[1], "--apropos") == 0)) {
        char query[MAN_QUERY_MAX];
        if (!man_join_args(argc, argv, 2, query, sizeof(query))) {
            solar_os_shell_diag_problem(io, "man -k", "search query is too long",
                                        "man -k QUERY", NULL);
            return;
        }
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
        docs_warn_if_outdated(io);
#endif
        (void)man_search(io, query);
        return;
    }
    if (argc != 2) {
        if (argc < 2) {
            solar_os_shell_diag_missing(io, "man", "topic", "man TOPIC | man -k QUERY | man --list");
        } else {
            solar_os_shell_diag_unexpected(io, "man", argv[2],
                                           "man TOPIC | man -k QUERY | man --list");
        }
        return;
    }

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    docs_warn_if_outdated(io);
#endif
    const solar_os_manual_page_t *page = solar_os_manual_find(argv[1]);
    if (page == NULL) {
        (void)man_search(io, argv[1]);
        return;
    }
    if (!man_launch_pager(ctx, io, page)) {
        man_print_page(io, page);
    }
}

#if SOLAR_OS_PACKAGE_SERVICE_DOCS
typedef struct {
    solar_os_shell_io_t *io;
    size_t row;
    bool row_valid;
    solar_os_docs_progress_stage_t last_stage;
    size_t last_page;
    uint8_t last_percent;
    bool last_known;
} docs_shell_progress_t;

typedef struct {
    docs_shell_progress_t progress;
    esp_err_t result;
    volatile bool done;
} docs_update_worker_t;

static const char *docs_progress_stage_name(
    solar_os_docs_progress_stage_t stage)
{
    switch (stage) {
    case SOLAR_OS_DOCS_PROGRESS_CATALOG:
        return "catalog";
    case SOLAR_OS_DOCS_PROGRESS_SIGNATURE:
        return "signature";
    case SOLAR_OS_DOCS_PROGRESS_ARCHIVE:
        return "archive";
    case SOLAR_OS_DOCS_PROGRESS_EXTRACTING:
        return "extracting";
    case SOLAR_OS_DOCS_PROGRESS_VERIFYING:
        return "verifying";
    case SOLAR_OS_DOCS_PROGRESS_ACTIVATING:
        return "activating";
    case SOLAR_OS_DOCS_PROGRESS_DONE:
        return "done";
    default:
        return "update";
    }
}

static void docs_render_progress_bar(solar_os_shell_io_t *io,
                                     const char *prefix,
                                     uint8_t percent)
{
    size_t cols = solar_os_shell_io_cols(io);
    if (cols < 16U) {
        cols = 80U;
    }
    const size_t line_budget = cols > 1U ? cols - 1U : cols;
    size_t prefix_len = prefix != NULL ? strlen(prefix) : 0U;
    const size_t fixed = prefix_len + 7U;
    size_t bar_width = line_budget > fixed ? line_budget - fixed : 1U;
    if (bar_width > DOCS_PROGRESS_BAR_WIDTH) {
        bar_width = DOCS_PROGRESS_BAR_WIDTH;
    }
    const uint8_t filled =
        (uint8_t)((percent * bar_width) / 100U);
    if (prefix_len > 0U) {
        solar_os_shell_io_write(io, prefix);
    }
    solar_os_shell_io_put_char(io, '[');
    for (size_t i = 0U; i < bar_width; i++) {
        solar_os_shell_io_put_char(io, i < filled ? '#' : '-');
    }
    solar_os_shell_io_printf(io, "] %3u%%", (unsigned)percent);
}

static void docs_shell_progress_cb(
    const solar_os_docs_progress_t *progress,
    void *user)
{
    docs_shell_progress_t *state = (docs_shell_progress_t *)user;
    if (progress == NULL || state == NULL || state->io == NULL) {
        return;
    }
    uint8_t percent = 0U;
    if (progress->total_known && progress->bytes_total > 0U) {
        size_t calculated =
            (progress->bytes_read * 100U) / progress->bytes_total;
        if (calculated > 100U) {
            calculated = 100U;
        }
        percent = (uint8_t)calculated;
    }
    if (progress->stage == SOLAR_OS_DOCS_PROGRESS_DONE) {
        percent = 100U;
    }
    const bool changed =
        !state->row_valid ||
        progress->stage != state->last_stage ||
        progress->page_index != state->last_page ||
        progress->total_known != state->last_known ||
        percent != state->last_percent;
    if (!changed) {
        return;
    }
    if (!state->row_valid) {
        state->row = solar_os_shell_io_cursor_row(state->io);
        state->row_valid = true;
    }
    solar_os_shell_io_set_cursor(state->io, state->row, 0U);
    solar_os_shell_io_clear_line_from(state->io, state->row, 0U);
    char prefix[48];
    const size_t cols = solar_os_shell_io_cols(state->io);
    if (progress->stage == SOLAR_OS_DOCS_PROGRESS_ARCHIVE) {
        snprintf(prefix,
                 sizeof(prefix),
                 cols > 0U && cols < 24U ? "zip " : "help archive ");
        docs_render_progress_bar(state->io, prefix, percent);
    } else if (progress->stage == SOLAR_OS_DOCS_PROGRESS_EXTRACTING) {
        if (cols >= 32U) {
            snprintf(prefix,
                     sizeof(prefix),
                     "help extract %03u/%03u ",
                     (unsigned)progress->page_index,
                     (unsigned)progress->page_count);
        } else if (cols >= 20U) {
            snprintf(prefix,
                     sizeof(prefix),
                     "extract %03u/%03u ",
                     (unsigned)progress->page_index,
                     (unsigned)progress->page_count);
        } else {
            snprintf(prefix,
                     sizeof(prefix),
                     "%03u/%03u ",
                     (unsigned)progress->page_index,
                     (unsigned)progress->page_count);
        }
        docs_render_progress_bar(state->io, prefix, percent);
    } else {
        if (progress->stage == SOLAR_OS_DOCS_PROGRESS_CATALOG ||
            progress->stage == SOLAR_OS_DOCS_PROGRESS_SIGNATURE ||
            progress->stage == SOLAR_OS_DOCS_PROGRESS_DONE) {
            if (cols > 0U && cols < 24U) {
                const char *short_name =
                    progress->stage == SOLAR_OS_DOCS_PROGRESS_CATALOG ? "cat" :
                    progress->stage == SOLAR_OS_DOCS_PROGRESS_SIGNATURE ? "sig" :
                    "done";
                snprintf(prefix, sizeof(prefix), "%s ", short_name);
            } else {
                snprintf(prefix,
                         sizeof(prefix),
                         "help %s ",
                         docs_progress_stage_name(progress->stage));
            }
            docs_render_progress_bar(state->io, prefix, percent);
        } else {
            solar_os_shell_io_printf(
                state->io,
                "help %s",
                docs_progress_stage_name(progress->stage));
        }
    }
    solar_os_shell_io_flush(state->io);
    state->last_stage = progress->stage;
    state->last_page = progress->page_index;
    state->last_percent = percent;
    state->last_known = progress->total_known;
}

static void docs_update_task(void *arg)
{
    docs_update_worker_t *worker = (docs_update_worker_t *)arg;
    if (worker != NULL) {
        worker->result =
            solar_os_docs_update(docs_shell_progress_cb, &worker->progress);
        worker->done = true;
    }
    solar_os_task_delete_internal(NULL);
}

static esp_err_t docs_run_update(solar_os_shell_io_t *io)
{
    docs_update_worker_t worker = {
        .progress = {
            .io = io,
        },
        .result = ESP_FAIL,
    };
    TaskHandle_t task = NULL;
    if (solar_os_task_create_pinned_internal(docs_update_task,
                                             "docs_update",
                                             DOCS_UPDATE_TASK_STACK,
                                             &worker,
                                             tskIDLE_PRIORITY + 1U,
                                             &task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    TickType_t delay = pdMS_TO_TICKS(DOCS_UPDATE_WAIT_MS);
    if (delay == 0U) {
        delay = 1U;
    }
    while (!worker.done) {
        vTaskDelay(delay);
    }
    return worker.result;
}

static void docs_print_status(solar_os_shell_io_t *io)
{
    solar_os_docs_status_t status;
    const esp_err_t err = solar_os_docs_get_status(&status);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "help: status failed: %s\n", solar_os_shell_error_text(err));
        return;
    }
    solar_os_shell_io_printf(io, "Firmware: %s\n", SOLAR_OS_VERSION);
    solar_os_shell_io_printf(io,
                             "External manual: %s\n",
                             status.available ? "active" : "embedded fallback");
    if (status.available) {
        solar_os_shell_io_printf(io,
                                 "Manual version: %s%s\n",
                                 status.manual_version,
                                 strcmp(status.manual_version,
                                        SOLAR_OS_VERSION) != 0 ?
                                     " (may be outdated)" : "");
        solar_os_shell_io_printf(io, "Revision: %s\n", status.revision);
        solar_os_shell_io_printf(io, "Pages: %u\n", (unsigned)status.page_count);
    }
    solar_os_shell_io_printf(io, "Updating: %s\n", status.updating ? "yes" : "no");
    if (status.last_error[0] != '\0') {
        solar_os_shell_io_printf(io, "Last error: %s\n", status.last_error);
    }
}
#endif

#if SOLAR_OS_PACKAGE_APP_DOCS
static bool docs_launch_browser(solar_os_context_t *ctx,
                                solar_os_shell_io_t *io,
                                int argc,
                                char **argv)
{
    const esp_err_t err =
        solar_os_context_request_launch(ctx, &solar_os_docs_app, argc, argv);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io,
                                 "help: launch failed: %s\n",
                                 solar_os_shell_error_text(err));
        return false;
    }
    solar_os_shell_session_prepare_foreground_launch(ctx, false);
    return true;
}
#endif

void solar_os_shell_cmd_help(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL) {
        return;
    }
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        docs_print_status(io);
        return;
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        solar_os_shell_io_writeln(io, "help: downloading signed manual");
        solar_os_shell_io_flush(io);
        const esp_err_t err = docs_run_update(io);
        solar_os_shell_io_newline(io);
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "help: manual updated");
            docs_print_status(io);
        } else {
            solar_os_shell_io_printf(io,
                                     "help: update failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        const esp_err_t err = solar_os_docs_reset();
        if (err == ESP_OK) {
            solar_os_shell_io_writeln(io, "help: using embedded manual");
        } else {
            solar_os_shell_io_printf(io,
                                     "help: reset failed: %s\n",
                                     solar_os_shell_error_text(err));
        }
        return;
    }
#else
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        solar_os_shell_io_writeln(io, "External manual: embedded");
        solar_os_shell_io_writeln(io, "Updates: unavailable on this build");
        return;
    }
    if (argc == 2 &&
        (strcmp(argv[1], "update") == 0 ||
         strcmp(argv[1], "reset") == 0)) {
        solar_os_shell_io_writeln(
            io,
            "help: signed refresh is unavailable on this build");
        return;
    }
#endif
#if SOLAR_OS_PACKAGE_APP_DOCS
    if (argc == 1 ||
        (argc == 2 && solar_os_manual_find(argv[1]) != NULL)) {
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
        docs_warn_if_outdated(io);
#endif
        (void)docs_launch_browser(ctx, io, argc, argv);
        return;
    }
#else
    if (argc == 1) {
        solar_os_shell_io_writeln(io, "Read a topic: man TOPIC");
        solar_os_shell_io_writeln(io, "Search:       man -k QUERY");
        solar_os_shell_io_writeln(io, "List topics:  man --list");
        return;
    }
    if (argc == 2 && solar_os_manual_find(argv[1]) != NULL) {
        solar_os_shell_cmd_man(ctx, argc, argv);
        return;
    }
#endif
    if (argc > 2) {
        solar_os_shell_diag_unexpected(io, "help", argv[2], "help [TOPIC|status|update|reset]");
        return;
    }
    static const char * const help_keywords[] = {"status", "update", "reset"};
    const char *suggestion = solar_os_shell_suggest(argv[1], help_keywords,
                                                    sizeof(help_keywords) / sizeof(help_keywords[0]));
    solar_os_shell_diag_unknown(io, "help", "topic or subcommand", argv[1], suggestion,
                                "help [TOPIC|status|update|reset]");
}
