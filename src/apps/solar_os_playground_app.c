#include "solar_os_playground_app.h"

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_config.h"
#include "solar_os_keys.h"
#if SOLAR_OS_PACKAGE_APP_LUA
#include "solar_os_lua.h"
#endif
#include "solar_os_playground.h"
#if SOLAR_OS_PACKAGE_APP_PYTHON
#include "solar_os_python.h"
#endif
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define PLAYGROUND_APP_TASK_STACK 16384U
#define PLAYGROUND_APP_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PLAYGROUND_APP_TASK_STACK);
#define PLAYGROUND_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)
#define PLAYGROUND_APP_SEARCH_MAX 48U

typedef enum {
    PLAYGROUND_NODE_CATEGORY,
    PLAYGROUND_NODE_APP,
} playground_node_kind_t;

typedef struct {
    playground_node_kind_t kind;
    size_t category_index;
    size_t app_index;
} playground_node_t;

typedef enum {
    PLAYGROUND_OPERATION_NONE,
    PLAYGROUND_OPERATION_REFRESH,
    PLAYGROUND_OPERATION_INSTALL,
    PLAYGROUND_OPERATION_UNINSTALL,
} playground_operation_t;

typedef struct {
    solar_os_tui_t tui;
    solar_os_context_t *ctx;
    size_t cursor;
    size_t top;
    uint32_t collapsed_categories;
    char search[PLAYGROUND_APP_SEARCH_MAX];
    bool searching;
    bool details;
    bool uninstall_prompt;
    bool tui_active;
    bool busy;
    volatile bool stop_requested;
    volatile bool task_done;
    TaskHandle_t task;
    playground_operation_t operation;
    solar_os_playground_target_t target;
    solar_os_playground_app_info_t operation_app;
    solar_os_playground_progress_t progress;
    esp_err_t operation_result;
    char status[96];
} playground_app_state_t;

static void *playground_state;
#define playground (*(playground_app_state_t *)playground_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("cross-core operation handoff lock")
static portMUX_TYPE playground_app_lock = portMUX_INITIALIZER_UNLOCKED;

static bool playground_parse_target(const char *value,
                                    solar_os_playground_target_t *target);

static solar_os_shell_io_t *playground_io(solar_os_context_t *ctx)
{
    return ctx != NULL ? solar_os_context_shell_io(ctx) : NULL;
}

static bool playground_contains_casefold(const char *text, const char *query)
{
    if (query == NULL || query[0] == '\0') {
        return true;
    }
    if (text == NULL) {
        return false;
    }
    const size_t query_len = strlen(query);
    for (const char *start = text; *start != '\0'; start++) {
        size_t i = 0U;
        while (i < query_len && start[i] != '\0' &&
               tolower((unsigned char)start[i]) ==
                   tolower((unsigned char)query[i])) {
            i++;
        }
        if (i == query_len) {
            return true;
        }
    }
    return false;
}

static bool playground_app_matches(
    const solar_os_playground_app_info_t *app)
{
    return app != NULL &&
        (playground_contains_casefold(app->name, playground.search) ||
         playground_contains_casefold(app->id, playground.search) ||
         playground_contains_casefold(app->description, playground.search) ||
         playground_contains_casefold(app->author, playground.search) ||
         playground_contains_casefold(app->tags, playground.search) ||
         playground_contains_casefold(app->category, playground.search));
}

static size_t playground_category_app_count(
    const solar_os_playground_category_t *category)
{
    if (category == NULL) {
        return 0U;
    }
    size_t count = 0U;
    const size_t app_count = solar_os_playground_app_count();
    for (size_t i = 0U; i < app_count; i++) {
        solar_os_playground_app_info_t app;
        if (solar_os_playground_get_app(i, &app) &&
            strcmp(app.category, category->id) == 0 &&
            playground_app_matches(&app)) {
            count++;
        }
    }
    return count;
}

static bool playground_category_collapsed(size_t category_index)
{
    if (playground.search[0] != '\0' || category_index >= 32U) {
        return false;
    }
    return (playground.collapsed_categories &
            (UINT32_C(1) << category_index)) != 0U;
}

static void playground_set_category_collapsed(size_t category_index,
                                              bool collapsed)
{
    if (category_index >= 32U) {
        return;
    }
    const uint32_t bit = UINT32_C(1) << category_index;
    if (collapsed) {
        playground.collapsed_categories |= bit;
    } else {
        playground.collapsed_categories &= ~bit;
    }
}

static void playground_collapse_all_but_first_populated(void)
{
    playground.collapsed_categories = UINT32_MAX;
    const size_t category_count = solar_os_playground_category_count();
    for (size_t i = 0U; i < category_count; i++) {
        solar_os_playground_category_t category;
        if (solar_os_playground_get_category(i, &category) &&
            playground_category_app_count(&category) > 0U) {
            playground_set_category_collapsed(i, false);
            break;
        }
    }
}

static size_t playground_visible_count(void)
{
    size_t visible = 0U;
    const size_t category_count = solar_os_playground_category_count();
    for (size_t category_index = 0U;
         category_index < category_count;
         category_index++) {
        solar_os_playground_category_t category;
        if (!solar_os_playground_get_category(category_index, &category)) {
            continue;
        }
        const size_t app_count = playground_category_app_count(&category);
        if (app_count == 0U) {
            continue;
        }
        visible++;
        if (!playground_category_collapsed(category_index)) {
            visible += app_count;
        }
    }
    return visible;
}

static bool playground_node_at(size_t visible_index,
                               playground_node_t *node)
{
    if (node == NULL) {
        return false;
    }
    const size_t category_count = solar_os_playground_category_count();
    const size_t app_count = solar_os_playground_app_count();
    for (size_t category_index = 0U;
         category_index < category_count;
         category_index++) {
        solar_os_playground_category_t category;
        if (!solar_os_playground_get_category(category_index, &category)) {
            continue;
        }
        const size_t matching = playground_category_app_count(&category);
        if (matching == 0U) {
            continue;
        }
        if (visible_index == 0U) {
            *node = (playground_node_t){
                .kind = PLAYGROUND_NODE_CATEGORY,
                .category_index = category_index,
            };
            return true;
        }
        visible_index--;
        if (playground_category_collapsed(category_index)) {
            continue;
        }
        for (size_t app_index = 0U; app_index < app_count; app_index++) {
            solar_os_playground_app_info_t app;
            if (!solar_os_playground_get_app(app_index, &app) ||
                strcmp(app.category, category.id) != 0 ||
                !playground_app_matches(&app)) {
                continue;
            }
            if (visible_index == 0U) {
                *node = (playground_node_t){
                    .kind = PLAYGROUND_NODE_APP,
                    .category_index = category_index,
                    .app_index = app_index,
                };
                return true;
            }
            visible_index--;
        }
    }
    return false;
}

static size_t playground_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&playground.tui);
    return rows > 2U ? rows - 2U : 0U;
}

static const char *playground_runtime_short(
    solar_os_playground_runtime_t runtime)
{
    return runtime == SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON ? "Py" : "Lua";
}

static void playground_app_marker(
    const solar_os_playground_app_info_t *app,
    char marker[4])
{
    char installed_version[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    if (!app->compatible) {
        strlcpy(marker, "[!]", 4U);
    } else if (!solar_os_playground_is_installed(
                   app, installed_version, sizeof(installed_version))) {
        strlcpy(marker, "[ ]", 4U);
    } else if (installed_version[0] != '\0' &&
               strcmp(installed_version, app->version) != 0) {
        strlcpy(marker, "[U]", 4U);
    } else {
        strlcpy(marker, "[I]", 4U);
    }
}

static void playground_render_header(void)
{
    char header[PLAYGROUND_APP_LINE_MAX];
    if (playground.searching || playground.search[0] != '\0') {
        snprintf(header,
                 sizeof(header),
                 "Playground /%s%s",
                 playground.search,
                 playground.searching ? "_" : "");
    } else if (playground.busy) {
        solar_os_playground_progress_t progress;
        portENTER_CRITICAL(&playground_app_lock);
        progress = playground.progress;
        portEXIT_CRITICAL(&playground_app_lock);
        if (progress.total_known && progress.bytes_total > 0U) {
            const unsigned percent =
                (unsigned)((uint64_t)progress.bytes_read * 100U /
                           progress.bytes_total);
            snprintf(header, sizeof(header), "Playground %u%%", percent);
        } else {
            strlcpy(header, "Playground working...", sizeof(header));
        }
    } else {
        strlcpy(header, "Playground", sizeof(header));
    }
    solar_os_tui_draw_title(&playground.tui, header, NULL);
}

static void playground_render_tree(void)
{
    const size_t rows = solar_os_tui_rows(&playground.tui);
    const size_t cols = solar_os_tui_cols(&playground.tui);
    const size_t visible_rows = playground_visible_rows();
    const size_t visible_count = playground_visible_count();
    if (playground.cursor >= visible_count && visible_count > 0U) {
        playground.cursor = visible_count - 1U;
    }
    if (playground.cursor < playground.top) {
        playground.top = playground.cursor;
    } else if (visible_rows > 0U &&
               playground.cursor >= playground.top + visible_rows) {
        playground.top = playground.cursor - visible_rows + 1U;
    }

    for (size_t row = 0U; row < visible_rows; row++) {
        const size_t index = playground.top + row;
        playground_node_t node;
        char line[PLAYGROUND_APP_LINE_MAX] = "";
        if (playground_node_at(index, &node)) {
            if (node.kind == PLAYGROUND_NODE_CATEGORY) {
                solar_os_playground_category_t category;
                if (solar_os_playground_get_category(
                        node.category_index, &category)) {
                    snprintf(
                        line,
                        sizeof(line),
                        "%c %s (%u)",
                        playground_category_collapsed(node.category_index) ?
                            '+' : '-',
                        category.title,
                        (unsigned)playground_category_app_count(&category));
                }
            } else {
                solar_os_playground_app_info_t app;
                if (solar_os_playground_get_app(node.app_index, &app)) {
                    char marker[4];
                    playground_app_marker(&app, marker);
                    snprintf(line,
                             sizeof(line),
                             "  %s %s  %s",
                             marker,
                             app.name,
                             playground_runtime_short(app.runtime));
                }
            }
        }
        const uint8_t attr = index == playground.cursor ?
            SOLAR_OS_TUI_ATTR_INVERSE : SOLAR_OS_TUI_ATTR_NORMAL;
        solar_os_tui_write_cell(&playground.tui, row + 1U, 0U, cols, line, attr);
    }
    for (size_t row = visible_rows + 1U; row + 1U < rows; row++) {
        solar_os_tui_write_cell(&playground.tui, row, 0U, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }

    const char *footer = playground.status[0] != '\0' ?
        playground.status :
        "/ search  Enter select  [i]nstall  [u]ninstall  [r]efresh  [q]uit";
    if (playground.uninstall_prompt) {
        footer = "Uninstall selected application? y/N";
    }
    if (rows > 1U) {
        solar_os_tui_draw_help(&playground.tui, footer);
    }
}

static bool playground_selected_app(solar_os_playground_app_info_t *app)
{
    playground_node_t node;
    return playground_node_at(playground.cursor, &node) &&
        node.kind == PLAYGROUND_NODE_APP &&
        solar_os_playground_get_app(node.app_index, app);
}

static void playground_render_details(void)
{
    const size_t rows = solar_os_tui_rows(&playground.tui);
    const size_t cols = solar_os_tui_cols(&playground.tui);
    solar_os_playground_app_info_t app;
    if (!playground_selected_app(&app)) {
        playground.details = false;
        playground_render_tree();
        return;
    }
    char line[PLAYGROUND_APP_LINE_MAX];
    char installed_version[SOLAR_OS_PLAYGROUND_VERSION_MAX];
    const bool installed = solar_os_playground_is_installed(
        &app, installed_version, sizeof(installed_version));
    size_t row = 1U;
    if (row + 1U < rows) {
        snprintf(line,
                 sizeof(line),
                 "%s %s  v%s",
                 app.name,
                 playground_runtime_short(app.runtime),
                 app.version);
        solar_os_tui_write_cell(&playground.tui, row++, 0U, cols, line, SOLAR_OS_TUI_ATTR_BOLD);
    }
    if (row + 1U < rows) {
        solar_os_tui_write_cell(&playground.tui,
            row++, 0U, cols, app.description, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (row + 1U < rows) {
        snprintf(line, sizeof(line), "by %s", app.author);
        solar_os_tui_write_cell(&playground.tui, row++, 0U, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    if (row + 1U < rows) {
        if (!app.compatible) {
            snprintf(line, sizeof(line), "Unavailable: %s", app.incompatibility);
        } else if (installed) {
            snprintf(line,
                     sizeof(line),
                     "Installed %s%s",
                     installed_version,
                     strcmp(installed_version, app.version) != 0 ?
                         " - update available" : "");
        } else {
            snprintf(line,
                     sizeof(line),
                     "Install size %u bytes",
                     (unsigned)app.size);
        }
        solar_os_tui_write_cell(&playground.tui, row++, 0U, cols, line, SOLAR_OS_TUI_ATTR_NORMAL);
    }
    while (row + 1U < rows) {
        solar_os_tui_write_cell(&playground.tui, row++, 0U, cols, "", SOLAR_OS_TUI_ATTR_NORMAL);
    }
    const char *footer = playground.status[0] != '\0' ?
        playground.status :
        (installed ? "[r]un  [i]nstall  [u]ninstall  Esc back" :
                     "[i]nstall  Esc back");
    if (playground.uninstall_prompt) {
        footer = "Uninstall selected application? y/N";
    }
    if (rows > 1U) {
        solar_os_tui_draw_help(&playground.tui, footer);
    }
}

static void playground_render(void)
{
    if (!playground.tui_active) {
        return;
    }
    solar_os_tui_clear(&playground.tui);
    playground_render_header();
    if (playground.details) {
        playground_render_details();
    } else {
        playground_render_tree();
    }
    solar_os_tui_set_cursor_visible(&playground.tui, playground.searching);
    if (playground.searching) {
        const size_t col = strlen("Playground /") + strlen(playground.search);
        solar_os_tui_move(&playground.tui, 0U, col);
    }
    solar_os_tui_refresh(&playground.tui);
}

static void playground_progress(
    const solar_os_playground_progress_t *progress,
    void *user)
{
    (void)user;
    if (progress == NULL) {
        return;
    }
    portENTER_CRITICAL(&playground_app_lock);
    playground.progress = *progress;
    portEXIT_CRITICAL(&playground_app_lock);
}

static void playground_operation_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    switch (playground.operation) {
    case PLAYGROUND_OPERATION_REFRESH:
        err = solar_os_playground_refresh(&playground.stop_requested,
                                          playground_progress,
                                          NULL);
        break;
    case PLAYGROUND_OPERATION_INSTALL:
        err = solar_os_playground_install(&playground.operation_app,
                                          playground.target,
                                          &playground.stop_requested,
                                          playground_progress,
                                          NULL);
        break;
    case PLAYGROUND_OPERATION_UNINSTALL:
        err = solar_os_playground_uninstall(&playground.operation_app,
                                            playground_progress,
                                            NULL);
        break;
    default:
        break;
    }
    portENTER_CRITICAL(&playground_app_lock);
    playground.operation_result = err;
    playground.task_done = true;
    portEXIT_CRITICAL(&playground_app_lock);
    solar_os_task_delete_internal(NULL);
}

static bool playground_start_operation(
    playground_operation_t operation,
    const solar_os_playground_app_info_t *app,
    solar_os_playground_target_t target)
{
    if (playground.busy) {
        strlcpy(playground.status, "operation already running", sizeof(playground.status));
        return false;
    }
    playground.operation = operation;
    playground.target = target;
    if (app != NULL) {
        playground.operation_app = *app;
    } else {
        memset(&playground.operation_app, 0, sizeof(playground.operation_app));
    }
    memset(&playground.progress, 0, sizeof(playground.progress));
    playground.operation_result = ESP_OK;
    playground.task_done = false;
    playground.stop_requested = false;
    playground.busy = true;
    playground.status[0] = '\0';
    const BaseType_t created = solar_os_task_create_pinned_internal(
        playground_operation_task,
        "playground",
        PLAYGROUND_APP_TASK_STACK,
        NULL,
        PLAYGROUND_APP_TASK_PRIORITY,
        &playground.task,
        tskNO_AFFINITY,
        SOLAR_OS_TASK_ROLE_FOREGROUND);
    if (created != pdPASS) {
        playground.busy = false;
        playground.task_done = true;
        strlcpy(playground.status, "worker could not start", sizeof(playground.status));
        return false;
    }
    return true;
}

static void playground_finish_operation(void)
{
    portENTER_CRITICAL(&playground_app_lock);
    const bool done = playground.task_done;
    const esp_err_t result = playground.operation_result;
    portEXIT_CRITICAL(&playground_app_lock);
    if (!playground.busy || !done) {
        return;
    }
    playground.busy = false;
    playground.task = NULL;
    if (result == ESP_OK) {
        const char *message = "done";
        if (playground.operation == PLAYGROUND_OPERATION_REFRESH) {
            message = "catalog refreshed";
            playground_collapse_all_but_first_populated();
            playground.cursor = 0U;
            playground.top = 0U;
        } else if (playground.operation == PLAYGROUND_OPERATION_INSTALL) {
            message = "application installed";
        } else if (playground.operation == PLAYGROUND_OPERATION_UNINSTALL) {
            message = "application uninstalled";
        }
        strlcpy(playground.status, message, sizeof(playground.status));
    } else if (playground.stop_requested) {
        strlcpy(playground.status, "cancelled", sizeof(playground.status));
    } else {
        snprintf(playground.status,
                 sizeof(playground.status),
                 "failed: %s",
                 esp_err_to_name(result));
    }
    const playground_operation_t completed_operation = playground.operation;
    playground.operation = PLAYGROUND_OPERATION_NONE;
    if (!playground.tui_active) {
        solar_os_shell_io_t *io = playground_io(playground.ctx);
        if (completed_operation == PLAYGROUND_OPERATION_INSTALL &&
            result == ESP_OK) {
            solar_os_shell_io_printf(io,
                                     "playground: installed %s\n",
                                     playground.operation_app.id);
        } else {
            solar_os_shell_io_printf(io, "playground: %s\n", playground.status);
        }
        solar_os_shell_io_flush(io);
        solar_os_context_finish(
            playground.ctx,
            result == ESP_OK ? 0 : (playground.stop_requested ? 130 : 1),
            NULL);
        return;
    }
    playground_render();
}

static void playground_move(int delta)
{
    const size_t count = playground_visible_count();
    if (delta < 0 && playground.cursor > 0U) {
        playground.cursor--;
    } else if (delta > 0 && playground.cursor + 1U < count) {
        playground.cursor++;
    }
    playground.status[0] = '\0';
}

static void playground_page(bool down)
{
    const size_t count = playground_visible_count();
    const size_t rows = playground_visible_rows();
    const size_t step = rows > 1U ? rows - 1U : 1U;
    if (down) {
        playground.cursor =
            playground.cursor + step < count ?
                playground.cursor + step :
                (count > 0U ? count - 1U : 0U);
    } else {
        playground.cursor =
            playground.cursor > step ? playground.cursor - step : 0U;
    }
    playground.status[0] = '\0';
}

static esp_err_t playground_launch_app(
    solar_os_context_t *ctx,
    const solar_os_playground_app_info_t *app)
{
    char path[SOLAR_OS_APP_ARG_LEN];
    if (app == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = solar_os_playground_entry_path(app, path, sizeof(path));
    if (err != ESP_OK) {
        return err;
    }
    char app_name[SOLAR_OS_APP_ARG_LEN];
    char app_path[SOLAR_OS_APP_ARG_LEN];
    char *argv[] = {app_name, app_path};
    const solar_os_app_t *runtime = NULL;
    strlcpy(app_path, path, sizeof(app_path));
    if (app->runtime == SOLAR_OS_PLAYGROUND_RUNTIME_PYTHON) {
#if SOLAR_OS_PACKAGE_APP_PYTHON
        runtime = &solar_os_python_app;
        strlcpy(app_name, "python", sizeof(app_name));
#endif
    } else {
#if SOLAR_OS_PACKAGE_APP_LUA
        runtime = &solar_os_lua_app;
        strlcpy(app_name, "lua", sizeof(app_name));
#endif
    }
    if (runtime == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return solar_os_context_request_launch_ex(
        ctx, runtime, 2, argv, SOLAR_OS_LAUNCH_CHILD_RETURN);
}

static bool playground_launch_selected(solar_os_context_t *ctx)
{
    solar_os_playground_app_info_t app;
    if (!playground_selected_app(&app)) {
        return false;
    }
    const esp_err_t err = playground_launch_app(ctx, &app);
    if (err != ESP_OK) {
        strlcpy(playground.status,
                err == ESP_ERR_NOT_FOUND ?
                    "application is not installed" :
                    "runtime is not available",
                sizeof(playground.status));
        return false;
    }
    return true;
}

static void playground_request_install(
    solar_os_playground_target_t target)
{
    solar_os_playground_app_info_t app;
    if (!playground_selected_app(&app)) {
        return;
    }
    if (!app.compatible) {
        strlcpy(playground.status, app.incompatibility, sizeof(playground.status));
        return;
    }
    (void)playground_start_operation(
        PLAYGROUND_OPERATION_INSTALL, &app, target);
}

static void playground_begin_install(void)
{
    playground_request_install(SOLAR_OS_PLAYGROUND_TARGET_AUTO);
}

static void playground_begin_uninstall(void)
{
    solar_os_playground_app_info_t app;
    if (!playground_selected_app(&app)) {
        return;
    }
    if (!solar_os_playground_is_installed(&app, NULL, 0U)) {
        strlcpy(playground.status,
                "application is not installed",
                sizeof(playground.status));
        return;
    }
    playground.uninstall_prompt = true;
    playground.status[0] = '\0';
}

static bool playground_handle_source_command(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || strcmp(solar_os_context_argv(ctx, 1), "source") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    esp_err_t err = ESP_OK;
    int exit_code = 0;
    if (argc == 2) {
        char source[SOLAR_OS_PLAYGROUND_SOURCE_URL_MAX];
        solar_os_playground_get_source(source, sizeof(source));
        solar_os_shell_io_printf(io, "playground source: %s\n", source);
    } else if (argc == 3 &&
               strcmp(solar_os_context_argv(ctx, 2), "reset") == 0) {
        err = solar_os_playground_reset_source();
        solar_os_shell_io_printf(io,
                                 "playground source: %s\n",
                                 err == ESP_OK ? "default restored" :
                                                 esp_err_to_name(err));
    } else if (argc == 3) {
        err = solar_os_playground_set_source(
            solar_os_context_argv(ctx, 2));
        solar_os_shell_io_printf(io,
                                 "playground source: %s\n",
                                 err == ESP_OK ? "saved" : esp_err_to_name(err));
    } else {
        solar_os_shell_io_writeln(
            io, "usage: playground source [repository-or-catalog-url|reset]");
        exit_code = 2;
    }
    if (err != ESP_OK) {
        exit_code = 1;
    }
    solar_os_shell_io_flush(io);
    solar_os_context_finish(ctx, exit_code, NULL);
    return true;
}

static void playground_command_finish(solar_os_context_t *ctx, int exit_code)
{
    solar_os_shell_io_flush(playground_io(ctx));
    solar_os_context_finish(ctx, exit_code, NULL);
}

static bool playground_handle_delete_command(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || strcmp(solar_os_context_argv(ctx, 1), "delete") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    int exit_code = 0;
    if (argc != 2) {
        solar_os_shell_io_writeln(io, "usage: playground delete");
        exit_code = 2;
    } else {
        const esp_err_t err = solar_os_playground_delete();
        solar_os_shell_io_printf(
            io,
            "playground: %s\n",
            err == ESP_OK ?
                "storage deleted; catalog cleared" :
                esp_err_to_name(err));
        exit_code = err == ESP_OK ? 0 : 1;
    }
    playground_command_finish(ctx, exit_code);
    return true;
}

static bool playground_handle_storage_command(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || strcmp(solar_os_context_argv(ctx, 1), "storage") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    int exit_code = 0;
    if (argc == 2) {
        solar_os_shell_io_printf(
            io,
            "playground storage: %s\n",
            solar_os_playground_target_name(
                solar_os_playground_get_storage()));
    } else {
        solar_os_playground_target_t target =
            SOLAR_OS_PLAYGROUND_TARGET_AUTO;
        const bool valid =
            argc == 3 &&
            playground_parse_target(solar_os_context_argv(ctx, 2), &target) &&
            target != SOLAR_OS_PLAYGROUND_TARGET_AUTO;
        const esp_err_t err = valid ?
            solar_os_playground_set_storage(target) : ESP_ERR_INVALID_ARG;
        if (!valid) {
            solar_os_shell_io_writeln(
                io, "usage: playground storage [flash|sd]");
            exit_code = 2;
        } else {
            solar_os_shell_io_printf(
                io,
                "playground storage: %s\n",
                err == ESP_OK ? "saved" : esp_err_to_name(err));
            exit_code = err == ESP_OK ? 0 : 1;
        }
    }
    playground_command_finish(ctx, exit_code);
    return true;
}

static bool playground_handle_reload_command(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || strcmp(solar_os_context_argv(ctx, 1), "reload") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    int exit_code = 0;
    if (argc != 2) {
        solar_os_shell_io_writeln(io, "usage: playground reload");
        exit_code = 2;
    } else {
        const esp_err_t err = solar_os_playground_reload();
        solar_os_shell_io_printf(
            io,
            "playground: %s\n",
            err == ESP_OK ? "local catalog loaded" : esp_err_to_name(err));
        exit_code = err == ESP_OK ? 0 : 1;
    }
    playground_command_finish(ctx, exit_code);
    return true;
}

static bool playground_catalog_required(solar_os_context_t *ctx)
{
    if (solar_os_playground_catalog_available()) {
        return true;
    }
    solar_os_shell_io_writeln(
        playground_io(ctx),
        "playground: catalog unavailable; run playground refresh");
    playground_command_finish(ctx, 1);
    return false;
}

static bool playground_join_query(solar_os_context_t *ctx,
                                  int first,
                                  char *query,
                                  size_t query_len)
{
    query[0] = '\0';
    size_t used = 0U;
    const int argc = solar_os_context_argc(ctx);
    for (int i = first; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        const size_t len = arg != NULL ? strlen(arg) : 0U;
        const size_t separator = used > 0U ? 1U : 0U;
        if (len == 0U || used + separator + len >= query_len) {
            return false;
        }
        if (separator != 0U) {
            query[used++] = ' ';
        }
        memcpy(query + used, arg, len);
        used += len;
        query[used] = '\0';
    }
    return used > 0U;
}

static bool playground_handle_search_command(solar_os_context_t *ctx)
{
    if (solar_os_context_argc(ctx) < 2 ||
        strcmp(solar_os_context_argv(ctx, 1), "search") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    if (!playground_join_query(
            ctx, 2, playground.search, sizeof(playground.search))) {
        solar_os_shell_io_writeln(io, "usage: playground search QUERY...");
        playground_command_finish(ctx, 2);
        return true;
    }
    if (!playground_catalog_required(ctx)) {
        return true;
    }

    size_t matches = 0U;
    const size_t count = solar_os_playground_app_count();
    for (size_t i = 0U; i < count; i++) {
        solar_os_playground_app_info_t app;
        if (!solar_os_playground_get_app(i, &app) ||
            !playground_app_matches(&app)) {
            continue;
        }
        char marker[4];
        playground_app_marker(&app, marker);
        solar_os_shell_io_printf(io,
                                 "%s %-31s %-6s %s\n",
                                 marker,
                                 app.id,
                                 solar_os_playground_runtime_name(app.runtime),
                                 app.name);
        matches++;
    }
    if (matches == 0U) {
        solar_os_shell_io_printf(
            io, "playground: no matches for %s\n", playground.search);
    }
    playground_command_finish(ctx, 0);
    return true;
}

static bool playground_parse_target(const char *value,
                                    solar_os_playground_target_t *target)
{
    if (target == NULL) {
        return false;
    }
    if (value == NULL || strcmp(value, "auto") == 0) {
        *target = SOLAR_OS_PLAYGROUND_TARGET_AUTO;
        return true;
    }
    if (strcmp(value, "flash") == 0) {
        *target = SOLAR_OS_PLAYGROUND_TARGET_FLASH;
        return true;
    }
    if (strcmp(value, "sd") == 0) {
        *target = SOLAR_OS_PLAYGROUND_TARGET_SD;
        return true;
    }
    return false;
}

static bool playground_handle_install_command(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    if (argc < 2 || strcmp(solar_os_context_argv(ctx, 1), "install") != 0) {
        return false;
    }
    solar_os_shell_io_t *io = playground_io(ctx);
    solar_os_playground_target_t target = SOLAR_OS_PLAYGROUND_TARGET_AUTO;
    if ((argc != 3 && argc != 4) ||
        !playground_parse_target(
            argc == 4 ? solar_os_context_argv(ctx, 3) : NULL, &target)) {
        solar_os_shell_io_writeln(
            io, "usage: playground install ID [auto|flash|sd]");
        playground_command_finish(ctx, 2);
        return true;
    }
    if (!playground_catalog_required(ctx)) {
        return true;
    }

    solar_os_playground_app_info_t app;
    const char *id = solar_os_context_argv(ctx, 2);
    if (!solar_os_playground_find_app(id, NULL, &app)) {
        solar_os_shell_io_printf(io, "playground: application not found: %s\n", id);
        playground_command_finish(ctx, 1);
        return true;
    }
    if (!app.compatible) {
        solar_os_shell_io_printf(
            io, "playground: %s is unavailable: %s\n", id, app.incompatibility);
        playground_command_finish(ctx, 1);
        return true;
    }

    solar_os_shell_io_printf(io, "playground: installing %s\n", id);
    solar_os_shell_io_flush(io);
    if (!playground_start_operation(PLAYGROUND_OPERATION_INSTALL, &app, target)) {
        solar_os_shell_io_writeln(io, "playground: install worker could not start");
        playground_command_finish(ctx, 1);
    }
    return true;
}

static esp_err_t playground_start(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    const char *subcommand = argc >= 2 ? solar_os_context_argv(ctx, 1) : NULL;
    const bool command_mode =
        subcommand != NULL && strcmp(subcommand, "refresh") != 0;
    if (command_mode) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
    }
    if (solar_os_playground_init() != ESP_OK) {
        return ESP_FAIL;
    }
    memset(&playground, 0, sizeof(playground));
    playground.ctx = ctx;
    if (playground_handle_source_command(ctx)) {
        return ESP_OK;
    }
    if (playground_handle_delete_command(ctx) ||
        playground_handle_storage_command(ctx) ||
        playground_handle_reload_command(ctx) ||
        playground_handle_search_command(ctx) ||
        playground_handle_install_command(ctx)) {
        return ESP_OK;
    }
    const bool force_refresh = argc == 2 &&
        strcmp(solar_os_context_argv(ctx, 1), "refresh") == 0;
    if (argc > 1 && !force_refresh) {
        solar_os_shell_io_t *io = playground_io(ctx);
        solar_os_shell_io_writeln(
            io,
            "usage: playground [delete|refresh|reload|search QUERY|install ID [target]|"
            "run ID [ARG...]|source [url|reset]|storage [flash|sd]]");
        solar_os_context_finish(ctx, 2, NULL);
        return ESP_OK;
    }

    esp_err_t reload_err = ESP_OK;
    if (!force_refresh) {
        reload_err = solar_os_playground_reload();
    }
    playground_collapse_all_but_first_populated();
    esp_err_t err = solar_os_tui_screen_begin(&playground.tui, ctx);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(
            playground_io(ctx),
            "playground: terminal is not TUI capable: %s\n",
            esp_err_to_name(err));
        solar_os_context_finish(ctx, 1, NULL);
        return ESP_OK;
    }
    playground.tui_active = true;
    if (force_refresh) {
        (void)playground_start_operation(
            PLAYGROUND_OPERATION_REFRESH, NULL, SOLAR_OS_PLAYGROUND_TARGET_AUTO);
    } else if (!solar_os_playground_catalog_available()) {
        snprintf(playground.status,
                 sizeof(playground.status),
                 "local catalog unavailable (%s) - press r to refresh",
                 esp_err_to_name(reload_err));
    }
    playground_render();
    return ESP_OK;
}

static void playground_resume(solar_os_context_t *ctx)
{
    playground.ctx = ctx;
    playground_render();
}

static void playground_cleanup_tui(void)
{
    if (!playground.tui_active) {
        return;
    }
    solar_os_tui_set_cursor_visible(&playground.tui, true);
    solar_os_tui_clear(&playground.tui);
    solar_os_tui_refresh(&playground.tui);
    solar_os_tui_end(&playground.tui);
    playground.tui_active = false;
}

static void playground_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    playground.stop_requested = true;
    solar_os_playground_cancel();
    if (playground.busy &&
        !solar_os_task_wait_done(playground.task,
                                 &playground.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        playground_cleanup_tui();
        return;
    }
    playground_cleanup_tui();
}

static bool playground_state_release_ready(void)
{
    return playground.task == NULL || playground.task_done;
}

static void playground_title(solar_os_context_t *ctx,
                             char *buffer,
                             size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "playground", buffer_len);
    }
}

static bool playground_handle_search(uint8_t ch)
{
    const size_t len = strlen(playground.search);
    if (ch == SOLAR_OS_KEY_ESCAPE) {
        playground.searching = false;
        playground.search[0] = '\0';
        playground.cursor = 0U;
        playground.top = 0U;
        return true;
    }
    if (ch == '\r' || ch == '\n') {
        playground.searching = false;
        return true;
    }
    if (ch == '\b' || ch == 0x7fU) {
        if (len > 0U) {
            playground.search[len - 1U] = '\0';
            playground.cursor = 0U;
            playground.top = 0U;
        }
        return true;
    }
    if (isprint(ch) && len + 1U < sizeof(playground.search)) {
        playground.search[len] = (char)ch;
        playground.search[len + 1U] = '\0';
        playground.cursor = 0U;
        playground.top = 0U;
    }
    return true;
}

static bool playground_event(solar_os_context_t *ctx,
                             const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        playground_finish_operation();
        if (playground.busy) {
            playground_render();
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }
    const uint8_t ch = (uint8_t)event->data.ch;
    if (playground.searching) {
        (void)playground_handle_search(ch);
        playground_render();
        return true;
    }
    if (playground.uninstall_prompt) {
        playground.uninstall_prompt = false;
        if (ch == 'y' || ch == 'Y') {
            solar_os_playground_app_info_t app;
            if (playground_selected_app(&app)) {
                (void)playground_start_operation(
                    PLAYGROUND_OPERATION_UNINSTALL,
                    &app,
                    SOLAR_OS_PLAYGROUND_TARGET_AUTO);
            }
        }
        playground_render();
        return true;
    }
    if (ch == SOLAR_OS_KEY_APP_EXIT ||
        (!playground.details &&
         (ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q'))) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (playground.busy) {
        return true;
    }
    if (playground.details) {
        if (ch == SOLAR_OS_KEY_ESCAPE || ch == SOLAR_OS_KEY_LEFT ||
            ch == 'q' || ch == 'Q') {
            playground.details = false;
        } else if (ch == 'i' || ch == 'I') {
            playground_begin_install();
        } else if (ch == 'u' || ch == 'U') {
            playground_begin_uninstall();
        } else if (ch == 'r' || ch == 'R') {
            if (playground_launch_selected(ctx)) {
                return true;
            }
        }
        playground_render();
        return true;
    }

    playground_node_t node;
    const bool selected = playground_node_at(playground.cursor, &node);
    switch (ch) {
    case '/':
        playground.searching = true;
        playground.search[0] = '\0';
        playground.cursor = 0U;
        playground.top = 0U;
        break;
    case SOLAR_OS_KEY_UP:
    case 'k':
        playground_move(-1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        playground_move(1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        playground_page(false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        playground_page(true);
        break;
    case SOLAR_OS_KEY_HOME:
        playground.cursor = 0U;
        break;
    case SOLAR_OS_KEY_END:
        if (playground_visible_count() > 0U) {
            playground.cursor = playground_visible_count() - 1U;
        }
        break;
    case SOLAR_OS_KEY_LEFT:
        if (selected && node.kind == PLAYGROUND_NODE_CATEGORY) {
            playground_set_category_collapsed(node.category_index, true);
        }
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (selected && node.kind == PLAYGROUND_NODE_CATEGORY) {
            playground_set_category_collapsed(node.category_index, false);
        } else if (selected) {
            playground.details = true;
        }
        break;
    case '\r':
    case '\n':
    case ' ':
        if (selected && node.kind == PLAYGROUND_NODE_CATEGORY) {
            playground_set_category_collapsed(
                node.category_index,
                !playground_category_collapsed(node.category_index));
        } else if (selected) {
            playground.details = true;
        }
        break;
    case 'r':
    case 'R':
        (void)playground_start_operation(
            PLAYGROUND_OPERATION_REFRESH,
            NULL,
            SOLAR_OS_PLAYGROUND_TARGET_AUTO);
        break;
    case 'i':
    case 'I':
        if (selected && node.kind == PLAYGROUND_NODE_APP) {
            playground_begin_install();
        }
        break;
    case 'u':
    case 'U':
        if (selected && node.kind == PLAYGROUND_NODE_APP) {
            playground_begin_uninstall();
        }
        break;
    default:
        return true;
    }
    playground_render();
    return true;
}

const solar_os_app_t solar_os_playground_app = {
    .name = "playground",
    .summary = "browse and run community Python and Lua apps",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = playground_start,
    .resume = playground_resume,
    .stop = playground_stop,
    .event = playground_event,
    .title = playground_title,
    .state_slot = &playground_state,
    .state_size = sizeof(playground_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = playground_state_release_ready,
    .state_release_cleanup = playground_cleanup_tui,
    .worker_stack_bytes = PLAYGROUND_APP_TASK_STACK,
};
