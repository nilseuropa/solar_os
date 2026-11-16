#include "solar_os_zoo_app.h"
#include "solar_os_file_shortcuts.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_keys.h"
#include "solar_os_shell_io.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"
#include "solar_os_zoo.h"

#define ZOO_STACK 16384U
#define ZOO_RECORDS_MAX (SOLAR_OS_ZOO_MODELS_MAX * 2U)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(ZOO_STACK);
typedef enum { ZOO_REFRESH, ZOO_INSTALL, ZOO_REMOVE } zoo_operation_t;
typedef struct { uint16_t model, count; bool collapsed; } zoo_folder_t;
typedef struct { uint16_t folder, model; } zoo_node_t;
typedef struct {
    solar_os_tui_t tui;
    solar_os_context_t *ctx;
    solar_os_zoo_t *zoo;
    solar_os_tui_viewport_t view;
    solar_os_tui_viewport_t detail_view;
    solar_os_tui_input_state_t input;
    char query[48], status[128];
    bool active, details, searching, busy, command, installed_only, confirming, result_popup;
    zoo_operation_t operation, prompt_operation;
    zoo_folder_t folders[ZOO_RECORDS_MAX];
    uint16_t model_folder[ZOO_RECORDS_MAX];
    zoo_node_t nodes[ZOO_RECORDS_MAX * 2U];
    size_t folder_count, model_count, node_count;
    size_t selected;
    volatile bool cancel, done;
    TaskHandle_t task;
    esp_err_t result;
    solar_os_zoo_progress_t progress, drawn;
    /* UI callbacks run on the caller's stack, including the main display task.
     * Keep catalog records and detail formatting in transient PSRAM state. */
    solar_os_zoo_model_t model, other;
    char identity[224], sizes[128], result_text[96], memory[128];
    char path[SOLAR_OS_STORAGE_PATH_MAX], detail_text[256];
} zoo_app_t;
static void *zoo_state;
#define app (*(zoo_app_t *)zoo_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("cross-core zoo progress handoff lock")
static portMUX_TYPE zoo_lock = portMUX_INITIALIZER_UNLOCKED;
static bool contains(const char *text, const char *query)
{
    if (!*query) return true;
    for (; *text; ++text) {
        size_t i = 0;
        while (query[i] && text[i] && tolower((unsigned char)query[i]) == tolower((unsigned char)text[i])) ++i;
        if (!query[i]) return true;
    }
    return false;
}
static bool accepts(size_t index, const solar_os_zoo_model_t *model)
{
    return (!app.installed_only || solar_os_zoo_installed(app.zoo, index)) &&
        (contains(model->name, app.query) || contains(model->id, app.query) ||
         contains(model->tasks, app.query) || contains(model->modalities, app.query) || contains(model->summary, app.query));
}
static size_t visible_rows(void)
{
    size_t rows = solar_os_tui_screen_content_rows(&app.tui, 1U, 1U);
    return app.searching && rows > 1 ? rows - 1 : rows;
}
static void tree_nodes(void)
{
    app.node_count = 0;
    for (size_t f = 0; f < app.folder_count; ++f) {
        app.nodes[app.node_count++] = (zoo_node_t){f, UINT16_MAX};
        if (app.folders[f].collapsed) continue;
        for (size_t i = 0; i < app.model_count; ++i)
            if (app.model_folder[i] == f) app.nodes[app.node_count++] = (zoo_node_t){f, i};
    }
    solar_os_tui_viewport_reconcile(&app.view, app.node_count, visible_rows());
}
static void tree_rebuild(void)
{
    app.folder_count = 0;
    app.model_count = solar_os_zoo_count(app.zoo);
    if (app.model_count > ZOO_RECORDS_MAX) app.model_count = ZOO_RECORDS_MAX;
    memset(app.model_folder, 0xff, sizeof(app.model_folder));
    for (size_t i = 0; i < app.model_count; ++i) {
        if (!solar_os_zoo_get(app.zoo, i, &app.model) || !accepts(i, &app.model)) continue;
        size_t f;
        for (f = 0; f < app.folder_count; ++f)
            if (solar_os_zoo_get(app.zoo, app.folders[f].model, &app.other) &&
                !strcmp(app.model.backend, app.other.backend) && !strcmp(app.model.target, app.other.target)) break;
        if (f == app.folder_count) {
            app.folders[f] = (zoo_folder_t){.model = i, .collapsed = f > 0 && !*app.query};
            ++app.folder_count;
        }
        ++app.folders[f].count;
        app.model_folder[i] = f;
    }
    app.view = (solar_os_tui_viewport_t){0};
    tree_nodes();
}
static bool selected_model(size_t *index)
{
    if (app.view.cursor >= app.node_count || app.nodes[app.view.cursor].model == UINT16_MAX) return false;
    *index = app.nodes[app.view.cursor].model;
    return true;
}
static void tree_left(void)
{
    if (app.view.cursor >= app.node_count) return;
    zoo_node_t node = app.nodes[app.view.cursor];
    if (node.model == UINT16_MAX) { app.folders[node.folder].collapsed = true; tree_nodes(); }
    else while (app.view.cursor && app.nodes[app.view.cursor].model != UINT16_MAX) --app.view.cursor;
}
static void line(size_t row, const char *text, bool selected)
{ solar_os_tui_write_cell(&app.tui, row, 0, solar_os_tui_cols(&app.tui), text, selected ? SOLAR_OS_TUI_ATTR_INVERSE : 0); }
static size_t detail_lines(const solar_os_zoo_model_t *model, size_t index, size_t top, bool draw)
{
    bool installed = solar_os_zoo_installed(app.zoo, index);
    snprintf(app.identity, sizeof(app.identity), "%s %s", model->id, model->version);
    if (model->local_only) snprintf(app.sizes, sizeof(app.sizes), "Local bundle; %s", model->license);
    else snprintf(app.sizes, sizeof(app.sizes), "%.1f MiB download / %.1f MiB installed; %s", model->archive_bytes/1048576.0, model->unpacked_bytes/1048576.0, model->license);
    snprintf(app.result_text, sizeof(app.result_text), "Result: %s; %s", model->result, installed ? "installed" : "not installed");
    if (model->memory_known) snprintf(app.memory, sizeof(app.memory), "Required free memory: %lu SRAM / %lu PSRAM bytes",
        (unsigned long)model->min_internal_bytes, (unsigned long)model->min_psram_bytes);
    else strlcpy(app.memory, "Free memory requirements: unreported", sizeof(app.memory));
    app.path[0] = 0;
    if (installed) solar_os_zoo_bundle_path(app.zoo, index, app.path, sizeof(app.path));
    const char *fields[] = {model->name, app.identity, model->reason, app.sizes, app.result_text,
        app.memory, model->validation, model->tasks, model->modalities, model->summary, app.path};
    size_t count = 0, cols = solar_os_tui_cols(&app.tui), visible = visible_rows();
    if (!cols) return 0;
    if (cols >= sizeof(app.detail_text)) cols = sizeof(app.detail_text) - 1;
    for (size_t f = 0; f < sizeof(fields)/sizeof(fields[0]); ++f) {
        const char *s = fields[f];
        while (*s) {
            size_t n = strlen(s); if (n > cols) n = cols;
            if (draw && count >= top && count - top < visible) {
                memcpy(app.detail_text, s, n); app.detail_text[n] = 0; line(1 + count - top, app.detail_text, count == 0);
            }
            ++count; s += n;
        }
    }
    return count;
}
static void render(void)
{
    if (!app.active) return;
    size_t rows = solar_os_tui_rows(&app.tui), cols = solar_os_tui_cols(&app.tui);
    solar_os_tui_clear(&app.tui);
    if (rows < 6 || cols < 20) { solar_os_tui_draw_too_small(&app.tui, "Zoo"); solar_os_tui_refresh(&app.tui); return; }
    char text[256]; snprintf(text, sizeof(text), "Zoo%s%s%s", *app.query ? " / " : "", app.query,
        app.installed_only ? " [installed]" : "");
    solar_os_tui_draw_title(&app.tui, text, solar_os_zoo_storage());
    /* A refresh/removal can replace catalog records. Do not read them while
     * the worker owns the service; only its progress handoff is shared. */
    if (!app.busy) {
        size_t index;
        solar_os_tui_viewport_reconcile(&app.view, app.node_count, visible_rows());
        if (app.details && selected_model(&index)) {
            solar_os_zoo_get(app.zoo, index, &app.model);
            size_t lines = detail_lines(&app.model, index, 0, false), visible = visible_rows();
            solar_os_tui_viewport_reconcile(&app.detail_view, lines > visible ? lines - visible + 1 : 1, 1);
            detail_lines(&app.model, index, app.detail_view.cursor, true);
        } else {
            for (size_t row = 0, pos = app.view.top; row < visible_rows() && pos < app.node_count; ++row, ++pos) {
                zoo_node_t node = app.nodes[pos];
                zoo_folder_t folder = app.folders[node.folder];
                bool is_folder = node.model == UINT16_MAX;
                index = is_folder ? folder.model : node.model;
                solar_os_zoo_get(app.zoo, index, &app.model);
                if (is_folder) snprintf(text, sizeof(text), "%c %s (%s; %u)", folder.collapsed ? '+' : '-',
                    app.model.target, app.model.backend, folder.count);
                else snprintf(text, sizeof(text), "  [%c] %s %s", solar_os_zoo_installed(app.zoo, index) ? 'I' :
                    !app.model.compatible ? '!' : ' ', app.model.name, app.model.version);
                line(row + 1, text, pos == app.view.cursor);
            }
            if (!app.node_count) line(2, "No models. R refresh; T installed filter.", false);
        }
    }
    solar_os_tui_draw_footer(&app.tui, app.status, app.busy ? "Esc cancel" :
        "Arrows tree  Enter open  I install  U remove  T installed  / search  R refresh  Q exit");
    const solar_os_tui_rect_t bounds = {.row = 1, .col = 0, .height =
        solar_os_tui_screen_content_rows(&app.tui, 1U, 1U), .width = cols};
    if (app.busy) {
        solar_os_zoo_progress_t p;
        portENTER_CRITICAL(&zoo_lock); p = app.progress; portEXIT_CRITICAL(&zoo_lock);
        snprintf(text, sizeof(text), "%s\n%s\n ", p.stage, app.cancel ? "Cancelling; please wait." : "Esc cancel");
        solar_os_tui_rect_t popup = {0};
        solar_os_tui_text_popup(&app.tui, &bounds, "Working", text, &popup);
        if (popup.height > 2 && popup.width > 4) solar_os_tui_progress_bar(&app.tui,
            popup.row + popup.height - 2, popup.col + 2, popup.width - 4, p.stage, p.bytes, p.total, p.total > 0);
        app.drawn = p;
    } else if (app.confirming) {
        solar_os_zoo_get(app.zoo, app.selected, &app.model);
        bool installing = app.prompt_operation == ZOO_INSTALL;
        snprintf(text, sizeof(text), "%s %s %s?\ny/N", installing ?
            solar_os_zoo_installed(app.zoo, app.selected) ? "Replace" : "Install" : "Remove",
            app.model.id, app.model.version);
        solar_os_tui_text_popup(&app.tui, &bounds, installing ? "Install bundle" : "Remove bundle", text, NULL);
    } else if (app.result_popup) {
        snprintf(text, sizeof(text), "%s\nEnter or Esc to close", app.status);
        solar_os_tui_text_popup(&app.tui, &bounds, !app.result ? "Success" : app.cancel ? "Cancelled" : "Failed", text, NULL);
    }
    bool input_visible = app.searching && !app.busy && !app.confirming && !app.result_popup;
    if (input_visible) solar_os_tui_draw_input(&app.tui,
        solar_os_tui_screen_content_end(&app.tui, 1U) - 1, 0, cols, "Search: ", app.query, &app.input, 0);
    solar_os_tui_set_cursor_visible(&app.tui, input_visible);
    solar_os_tui_refresh(&app.tui);
}
static void progress(const solar_os_zoo_progress_t *p, void *user)
{ zoo_app_t *s = user; portENTER_CRITICAL(&zoo_lock); s->progress = *p; portEXIT_CRITICAL(&zoo_lock); }
static void worker(void *user)
{
    zoo_app_t *s = user;
    esp_err_t error = s->operation == ZOO_REFRESH ? solar_os_zoo_refresh(s->zoo, &s->cancel, progress, s) :
        s->operation == ZOO_REMOVE ? solar_os_zoo_remove(s->zoo, s->selected, &s->cancel, progress, s) :
        solar_os_zoo_install(s->zoo, s->selected, &s->cancel, progress, s);
    portENTER_CRITICAL(&zoo_lock); s->result = error; s->done = true; portEXIT_CRITICAL(&zoo_lock);
    solar_os_task_delete_internal(NULL);
}
static void begin(zoo_operation_t operation, size_t index)
{
    if (app.busy) return;
    app.operation = operation; app.confirming = app.result_popup = false; app.selected = index; app.done = false; app.cancel = false; app.busy = true;
    memset(&app.progress, 0, sizeof(app.progress));
    strlcpy(app.progress.stage, operation == ZOO_REFRESH ? "catalog" : operation == ZOO_REMOVE ? "remove" : "download", sizeof(app.progress.stage));
    if (solar_os_task_create_pinned_internal(worker, "zoo", ZOO_STACK, &app, tskIDLE_PRIORITY + 2,
        &app.task, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        app.busy = false; app.done = true; app.result = ESP_ERR_NO_MEM; app.result_popup = !app.command;
        strlcpy(app.status, "worker could not start", sizeof(app.status));
        if (app.command) solar_os_context_finish(app.ctx, 1, NULL);
    }
}
static esp_err_t start(solar_os_context_t *ctx)
{
    memset(&app, 0, sizeof(app)); app.ctx = ctx; app.done = true;
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    int argc = solar_os_context_argc(ctx); const char *cmd = argc > 1 ? solar_os_context_argv(ctx, 1) : NULL;
    if (cmd && (!strcmp(cmd, "source") || !strcmp(cmd, "storage"))) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
        esp_err_t error = ESP_OK;
        if (argc > 3) error = ESP_ERR_INVALID_ARG;
        else if (argc == 3) error = !strcmp(cmd, "source") ? solar_os_zoo_set_source(
            !strcmp(solar_os_context_argv(ctx, 2), "reset") ? SOLAR_OS_ZOO_DEFAULT_SOURCE : solar_os_context_argv(ctx, 2)) :
            solar_os_zoo_set_storage(solar_os_context_argv(ctx, 2));
        if (!error && !strcmp(cmd, "source")) { char source[SOLAR_OS_ZOO_SOURCE_MAX]; solar_os_zoo_get_source(source, sizeof(source)); solar_os_shell_io_writeln(io, source); }
        else if (!error) solar_os_shell_io_writeln(io, solar_os_zoo_storage());
        if (error) solar_os_shell_io_printf(io, "zoo: %s\n", esp_err_to_name(error));
        solar_os_context_finish(ctx, error ? 1 : 0, NULL); return ESP_OK;
    }
    esp_err_t error = solar_os_zoo_open(&app.zoo);
    if (error) { solar_os_shell_io_printf(io, "zoo: storage unavailable: %s\n", esp_err_to_name(error)); solar_os_context_finish(ctx, 1, NULL); return ESP_OK; }
    error = solar_os_zoo_reload(app.zoo);
    if (error) snprintf(app.status, sizeof(app.status), "No cached catalog (%s); r refresh", esp_err_to_name(error));
    if (cmd && !strcmp(cmd, "installed") && argc == 2) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND);
        for (size_t i = 0; i < solar_os_zoo_count(app.zoo); ++i) if (solar_os_zoo_installed(app.zoo, i)) {
            solar_os_zoo_get(app.zoo, i, &app.model);
            solar_os_zoo_bundle_path(app.zoo, i, app.path, sizeof(app.path));
            solar_os_shell_io_printf(io, "%s %s [%s/%s] %s\n", app.model.id, app.model.version, app.model.backend, app.model.target, app.path);
        }
        solar_os_context_finish(ctx, error ? 1 : 0, NULL); return ESP_OK;
    }
    if (cmd && (!strcmp(cmd, "install") || !strcmp(cmd, "remove")) && argc == 4) {
        solar_os_context_set_app_class(ctx, SOLAR_OS_APP_CLASS_COMMAND); app.command = true;
        for (size_t i = 0; i < solar_os_zoo_count(app.zoo); ++i) {
            solar_os_zoo_get(app.zoo, i, &app.model);
            if (!strcmp(app.model.id, solar_os_context_argv(ctx, 2)) && !strcmp(app.model.version, solar_os_context_argv(ctx, 3))) {
                begin(!strcmp(cmd, "remove") ? ZOO_REMOVE : ZOO_INSTALL, i); return ESP_OK;
            }
        }
        solar_os_shell_io_writeln(io, "zoo: model/version not found; refresh the catalog"); solar_os_context_finish(ctx, 1, NULL); return ESP_OK;
    }
    if (cmd && (strcmp(cmd, "refresh") || argc != 2)) { solar_os_shell_io_writeln(io, "usage: zoo [refresh|source [URL|reset]|storage [sd|flash]|installed|install ID VERSION|remove ID VERSION]"); solar_os_context_finish(ctx, 2, NULL); return ESP_OK; }
    error = solar_os_tui_screen_begin(&app.tui, ctx);
    if (error) { solar_os_context_finish(ctx, 1, "zoo: terminal is not TUI capable"); return ESP_OK; }
    app.active = true;
    tree_rebuild();
    if (cmd) begin(ZOO_REFRESH, 0);
    render(); return ESP_OK;
}
static void resume(solar_os_context_t *ctx) { app.ctx = ctx; render(); }
static void stop(solar_os_context_t *ctx)
{ (void)ctx; app.cancel = true; if (app.busy) solar_os_task_wait_done(app.task, &app.done, SOLAR_OS_TASK_STOP_WAIT_MS); }
static bool ready(void) { return !app.busy || app.done; }
static void cleanup(void)
{
    if (app.active) { solar_os_tui_set_cursor_visible(&app.tui, true); solar_os_tui_clear(&app.tui); solar_os_tui_refresh(&app.tui); solar_os_tui_end(&app.tui); app.active = false; }
    solar_os_zoo_close(app.zoo); app.zoo = NULL;
}
static bool event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event->type == SOLAR_OS_EVENT_TICK) {
        bool redraw = false;
        portENTER_CRITICAL(&zoo_lock);
        bool done = app.done; esp_err_t result = app.result;
        solar_os_zoo_progress_t p = app.progress;
        portEXIT_CRITICAL(&zoo_lock);
        if (app.busy && done) {
            app.busy = false; app.task = NULL;
            snprintf(app.status, sizeof(app.status), "%s", result ? app.cancel ? "cancelled" : esp_err_to_name(result) :
                app.operation == ZOO_REFRESH ? "catalog refreshed" : app.operation == ZOO_REMOVE ? "stored bundle removed" : "bundle installed on storage");
            if (!result && app.operation != ZOO_INSTALL) { tree_rebuild(); app.details = false; }
            app.result_popup = !app.command;
            if (app.command) { solar_os_shell_io_writeln(solar_os_context_shell_io(ctx), app.status); solar_os_context_finish(ctx, result ? 1 : 0, NULL); }
            redraw = true;
        } else if (app.busy && (strcmp(p.stage, app.drawn.stage) || p.bytes/16384 != app.drawn.bytes/16384 || p.total != app.drawn.total)) redraw = true;
        if (redraw) render();
        return true;
    }
    uint8_t ch;
    bool search = false;
    if (event->type == SOLAR_OS_EVENT_CHAR) ch = event->data.ch;
    else if (event->type == SOLAR_OS_EVENT_KEY) {
        if (event->data.key.action == SOLAR_OS_INPUT_KEY_RELEASE) return true;
        solar_os_file_shortcut_t shortcut = solar_os_file_shortcut_from_key_event(&event->data.key);
        search = shortcut == SOLAR_OS_FILE_SHORTCUT_SEARCH;
        if (shortcut == SOLAR_OS_FILE_SHORTCUT_FULLSCREEN) {
            solar_os_tui_screen_key(&app.tui, SOLAR_OS_KEY_ALT_PREFIX);
            solar_os_tui_screen_key(&app.tui, SOLAR_OS_KEY_ENTER);
            render(); return true;
        }
        ch = event->data.key.key;
    } else return true;
    if (ch == '\r' || ch == '\n') ch = SOLAR_OS_KEY_ENTER;
    if (ch == SOLAR_OS_KEY_APP_EXIT) { app.cancel = true; solar_os_context_finish(ctx, 0, NULL); return true; }
    solar_os_tui_screen_key_action_t screen_key = solar_os_tui_screen_key(&app.tui, ch);
    if (screen_key == SOLAR_OS_TUI_SCREEN_KEY_TOGGLED) { render(); return true; }
    if (screen_key == SOLAR_OS_TUI_SCREEN_KEY_CONSUMED) return true;
    search |= screen_key == SOLAR_OS_TUI_SCREEN_KEY_PASSTHROUGH && (ch == 's' || ch == 'S');
    if (app.busy) { if (ch == 'c' || ch == 0x03 || ch == SOLAR_OS_KEY_ESCAPE) { app.cancel = true; render(); } return true; }
    if (app.confirming) {
        if (ch == 'y' || ch == 'Y') begin(app.prompt_operation, app.selected);
        else if (ch == 'n' || ch == 'N' || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ENTER) app.confirming = false;
        render(); return true;
    }
    if (app.result_popup) {
        if (ch == SOLAR_OS_KEY_ENTER || ch == SOLAR_OS_KEY_ESCAPE || ch == 'q' || ch == 'Q') app.result_popup = false;
        render(); return true;
    }
    if (!app.searching && (search || ch == '/')) {
        app.details = false; app.searching = true; app.input.cursor = strlen(app.query);
        render(); return true;
    }
    if (app.searching) {
        solar_os_tui_input_action_t action = solar_os_tui_input_key(app.query, sizeof(app.query), &app.input, ch, solar_os_tui_cols(&app.tui));
        if (action == SOLAR_OS_TUI_INPUT_CANCEL || action == SOLAR_OS_TUI_INPUT_SUBMIT) app.searching = false;
        if (action == SOLAR_OS_TUI_INPUT_CANCEL) app.query[0] = 0;
        tree_rebuild(); render(); return true;
    }
    size_t index = 0;
    bool selected = selected_model(&index);
    if (ch == 'q' || ch == 'Q' || ch == SOLAR_OS_KEY_ESCAPE || (app.details && ch == SOLAR_OS_KEY_LEFT)) {
        if (app.details) app.details = false;
        else solar_os_context_finish(ctx, 0, NULL);
    }
    else if (ch == 'r' || ch == 'R') begin(ZOO_REFRESH, 0);
    else if (ch == 't' || ch == 'T') { app.installed_only = !app.installed_only; app.details = false; tree_rebuild(); }
    else if (selected && (ch == 'i' || ch == 'I' || ch == 'd' || ch == 'D' || ch == 'u' || ch == 'U' || ch == 'x' || ch == 'X')) {
        bool installing = ch == 'i' || ch == 'I' || ch == 'd' || ch == 'D';
        solar_os_zoo_get(app.zoo, index, &app.model);
        if ((installing && app.model.downloadable) || (!installing && solar_os_zoo_installed(app.zoo, index))) {
            app.selected = index; app.prompt_operation = installing ? ZOO_INSTALL : ZOO_REMOVE; app.confirming = true;
        } else {
            app.result = installing ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_NOT_FOUND;
            app.cancel = false; app.result_popup = true;
            strlcpy(app.status, installing ? app.model.reason : "bundle is not installed", sizeof(app.status));
        }
    }
    else if (!app.details && (ch == SOLAR_OS_KEY_ENTER || ch == SOLAR_OS_KEY_RIGHT) && app.view.cursor < app.node_count) {
        zoo_node_t node = app.nodes[app.view.cursor];
        if (node.model == UINT16_MAX) {
            app.folders[node.folder].collapsed = ch == SOLAR_OS_KEY_RIGHT ? false : !app.folders[node.folder].collapsed;
            tree_nodes();
        } else app.details = true;
        memset(&app.detail_view, 0, sizeof(app.detail_view));
    }
    else if (!app.details && ch == SOLAR_OS_KEY_LEFT) tree_left();
    else if (app.details && selected) {
        solar_os_zoo_get(app.zoo, index, &app.model);
        size_t lines = detail_lines(&app.model, index, 0, false), visible = visible_rows();
        solar_os_tui_viewport_key(&app.detail_view, ch, lines > visible ? lines - visible + 1 : 1, visible, false);
    }
    else if (!app.details) solar_os_tui_viewport_key(&app.view, ch, app.node_count, visible_rows(), false);
    render(); return true;
}
static void title(solar_os_context_t *ctx, char *buffer, size_t size) { (void)ctx; strlcpy(buffer, "Zoo", size); }
const solar_os_app_t solar_os_zoo_app = {.name = "zoo", .summary = "browse and install model bundles",
    .app_class = SOLAR_OS_APP_CLASS_TUI, .flags = SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_KEY_EVENTS,
    .start = start, .resume = resume, .stop = stop, .event = event, .title = title,
    .state_slot = &zoo_state, .state_size = sizeof(zoo_app_t), .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = ready, .state_release_cleanup = cleanup, .worker_stack_bytes = ZOO_STACK};
