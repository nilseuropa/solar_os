#include "solar_os_shell_tui_apps.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_keys.h"
#include "solar_os_module_packages.h"
#include "solar_os_task.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define PKG_TUI_TASK_STACK (16U * 1024U)
#define PKG_TUI_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(PKG_TUI_TASK_STACK);

typedef enum {
    PKG_TUI_OPERATION_NONE = 0,
    PKG_TUI_OPERATION_REFRESH,
    PKG_TUI_OPERATION_INSTALL,
    PKG_TUI_OPERATION_REMOVE,
} pkg_tui_operation_t;

typedef struct {
    solar_os_tui_t tui;
    solar_os_context_t *ctx;
    solar_os_tui_viewport_t viewport;
    solar_os_module_catalog_t *catalog;
    solar_os_module_catalog_t *operation_catalog;
    solar_os_module_package_t operation_package;
    solar_os_module_install_result_t install_result;
    TaskHandle_t task;
    pkg_tui_operation_t operation;
    esp_err_t operation_result;
    uint32_t progress_bytes;
    uint32_t progress_total;
    uint32_t rendered_progress_bytes;
    uint32_t rendered_progress_total;
    char detail[128];
    char status[96];
    uint32_t collapsed_folders;
    pkg_tui_operation_t prompt_operation;
    bool details;
    bool result_popup;
    bool tui_active;
    bool busy;
    volatile bool stop_requested;
    volatile bool task_done;
} pkg_tui_state_t;

static void *pkg_tui_state;
#define pkg_tui (*(pkg_tui_state_t *)pkg_tui_state)
SOLAR_OS_APP_STATIC_SRAM_EXCEPTION("cross-core package operation handoff lock")
static portMUX_TYPE pkg_tui_lock = portMUX_INITIALIZER_UNLOCKED;

static void pkg_tui_render(void);

/* Match the category tree used by Playground and Flash. Keep all module kinds
 * visible, including empty folders. */
static const struct { solar_os_module_type_t type; const char *title; } pkg_folders[] = {
    {SOLAR_OS_MODULE_TYPE_APP, "Apps"}, {SOLAR_OS_MODULE_TYPE_JOB, "Jobs"},
    {SOLAR_OS_MODULE_TYPE_LIBRARY, "Libraries"}, {SOLAR_OS_MODULE_TYPE_DRIVER, "Drivers"},
};
typedef struct { size_t folder, package; bool is_folder; } pkg_tui_node_t;

static size_t pkg_tui_folder_count(size_t folder)
{
    size_t count = 0;
    for (size_t i = 0; pkg_tui.catalog && i < pkg_tui.catalog->count; ++i)
        count += pkg_tui.catalog->packages[i].type == pkg_folders[folder].type;
    return count;
}

static size_t pkg_tui_visible_count(void)
{
    size_t count = 0;
    for (size_t f = 0; f < 4; ++f) {
        size_t children = pkg_tui_folder_count(f);
        count += 1 + ((pkg_tui.collapsed_folders & (1U << f)) ? 0 : children);
    }
    return count;
}

static bool pkg_tui_node_at(size_t index, pkg_tui_node_t *node)
{
    for (size_t f = 0; f < 4; ++f) {
        if (!index) { *node = (pkg_tui_node_t){.folder=f, .is_folder=true}; return true; }
        --index;
        if (pkg_tui.collapsed_folders & (1U << f)) continue;
        for (size_t i = 0; pkg_tui.catalog && i < pkg_tui.catalog->count; ++i) {
            if (pkg_tui.catalog->packages[i].type != pkg_folders[f].type) continue;
            if (!index) { *node = (pkg_tui_node_t){.folder=f, .package=i}; return true; }
            --index;
        }
    }
    return false;
}

static void pkg_tui_open_first_folder(void)
{
    pkg_tui.collapsed_folders = UINT32_MAX;
    for (size_t f = 0; f < 4; ++f)
        if (pkg_tui_folder_count(f)) { pkg_tui.collapsed_folders &= ~(1U << f); break; }
}

static void pkg_tui_tree_left(void)
{
    pkg_tui_node_t node;
    if (!pkg_tui_node_at(pkg_tui.viewport.cursor, &node)) return;
    if (node.is_folder) pkg_tui.collapsed_folders |= 1U << node.folder;
    else {
        while (pkg_tui.viewport.cursor) {
            --pkg_tui.viewport.cursor;
            pkg_tui_node_at(pkg_tui.viewport.cursor, &node);
            if (node.is_folder) break;
        }
    }
}

static bool pkg_tui_selected(solar_os_module_package_t *package)
{
    pkg_tui_node_t node;
    if (!pkg_tui_node_at(pkg_tui.viewport.cursor, &node) || node.is_folder) {
        return false;
    }
    if (package != NULL) {
        *package = pkg_tui.catalog->packages[node.package];
    }
    return true;
}

static bool pkg_tui_cancelled(void *user)
{
    (void)user;
    return pkg_tui.stop_requested;
}

static void pkg_tui_progress(uint32_t bytes, uint32_t total, void *user)
{
    (void)user;
    portENTER_CRITICAL(&pkg_tui_lock);
    pkg_tui.progress_bytes = bytes;
    pkg_tui.progress_total = total;
    portEXIT_CRITICAL(&pkg_tui_lock);
}

static void pkg_tui_operation_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_ERR_INVALID_STATE;
    solar_os_module_catalog_t *catalog = NULL;
    solar_os_module_install_result_t install_result = {0};
    char detail[128] = {0};

    switch (pkg_tui.operation) {
    case PKG_TUI_OPERATION_REFRESH:
        err = solar_os_module_catalog_fetch_ex(&catalog,
                                               pkg_tui_cancelled,
                                               NULL,
                                               detail,
                                               sizeof(detail));
        break;
    case PKG_TUI_OPERATION_INSTALL: {
        const solar_os_module_install_options_t options = {
            .should_cancel = pkg_tui_cancelled,
            .progress = pkg_tui_progress,
        };
        err = solar_os_module_package_install(pkg_tui.operation_package.id,
                                              &options,
                                              &install_result,
                                              detail,
                                              sizeof(detail));
        break;
    }
    case PKG_TUI_OPERATION_REMOVE:
        err = solar_os_module_package_remove(pkg_tui.operation_package.id,
                                             detail,
                                             sizeof(detail));
        break;
    default:
        break;
    }

    portENTER_CRITICAL(&pkg_tui_lock);
    pkg_tui.operation_catalog = catalog;
    pkg_tui.install_result = install_result;
    pkg_tui.operation_result = err;
    strlcpy(pkg_tui.detail, detail, sizeof(pkg_tui.detail));
    pkg_tui.task_done = true;
    portEXIT_CRITICAL(&pkg_tui_lock);
    solar_os_task_delete_internal(NULL);
}

static bool pkg_tui_start_operation(pkg_tui_operation_t operation,
                                    const solar_os_module_package_t *package)
{
    if (pkg_tui.busy) {
        return false;
    }
    pkg_tui.operation = operation;
    if (package != NULL) {
        pkg_tui.operation_package = *package;
    } else {
        memset(&pkg_tui.operation_package, 0, sizeof(pkg_tui.operation_package));
    }
    pkg_tui.operation_result = ESP_OK;
    pkg_tui.progress_bytes = 0U;
    pkg_tui.progress_total = 0U;
    pkg_tui.detail[0] = '\0';
    pkg_tui.status[0] = '\0';
    pkg_tui.prompt_operation = PKG_TUI_OPERATION_NONE;
    pkg_tui.result_popup = false;
    pkg_tui.stop_requested = false;
    pkg_tui.task_done = false;
    pkg_tui.busy = true;

    if (solar_os_task_create_pinned_internal(pkg_tui_operation_task,
                                             "pkg_tui",
                                             PKG_TUI_TASK_STACK,
                                             NULL,
                                             PKG_TUI_TASK_PRIORITY,
                                             &pkg_tui.task,
                                             tskNO_AFFINITY,
                                             SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        pkg_tui.busy = false;
        pkg_tui.task_done = true;
        pkg_tui.task = NULL;
        pkg_tui.operation_result = ESP_ERR_NO_MEM;
        pkg_tui.result_popup = true;
        strlcpy(pkg_tui.status, "worker could not start", sizeof(pkg_tui.status));
        return false;
    }
    return true;
}

static void pkg_tui_update_installed(const char *id, bool installed)
{
    if (pkg_tui.catalog == NULL || id == NULL) {
        return;
    }
    for (size_t i = 0U; i < pkg_tui.catalog->count; i++) {
        if (strcmp(pkg_tui.catalog->packages[i].id, id) == 0) {
            pkg_tui.catalog->packages[i].installed = installed;
            return;
        }
    }
}

static void pkg_tui_finish_operation(void)
{
    portENTER_CRITICAL(&pkg_tui_lock);
    const bool done = pkg_tui.task_done;
    const esp_err_t result = pkg_tui.operation_result;
    solar_os_module_catalog_t *catalog = pkg_tui.operation_catalog;
    pkg_tui.operation_catalog = NULL;
    portEXIT_CRITICAL(&pkg_tui_lock);
    if (!pkg_tui.busy || !done) {
        return;
    }

    const pkg_tui_operation_t completed = pkg_tui.operation;
    pkg_tui.busy = false;
    pkg_tui.task = NULL;
    pkg_tui.operation = PKG_TUI_OPERATION_NONE;
    if (result == ESP_OK) {
        if (completed == PKG_TUI_OPERATION_REFRESH) {
            bool first_catalog = pkg_tui.catalog == NULL;
            solar_os_module_catalog_free(pkg_tui.catalog);
            pkg_tui.catalog = catalog;
            catalog = NULL;
            pkg_tui.viewport = (solar_os_tui_viewport_t){0};
            pkg_tui.details = false;
            if (first_catalog) pkg_tui_open_first_folder();
            strlcpy(pkg_tui.status, pkg_tui.detail[0] ? pkg_tui.detail :
                "signed catalog refreshed and saved", sizeof(pkg_tui.status));
        } else if (completed == PKG_TUI_OPERATION_INSTALL) {
            pkg_tui_update_installed(pkg_tui.operation_package.id, true);
            strlcpy(pkg_tui.status, "module installed", sizeof(pkg_tui.status));
        } else if (completed == PKG_TUI_OPERATION_REMOVE) {
            pkg_tui_update_installed(pkg_tui.operation_package.id, false);
            strlcpy(pkg_tui.status, "module removed", sizeof(pkg_tui.status));
        }
    } else if (pkg_tui.stop_requested) {
        strlcpy(pkg_tui.status, "cancelled", sizeof(pkg_tui.status));
    } else if (pkg_tui.detail[0] != '\0') {
        snprintf(pkg_tui.status,
                 sizeof(pkg_tui.status),
                 "failed: %.86s",
                 pkg_tui.detail);
    } else {
        snprintf(pkg_tui.status,
                 sizeof(pkg_tui.status),
                 "failed: %s",
                 esp_err_to_name(result));
    }
    solar_os_module_catalog_free(catalog);
    pkg_tui.result_popup = true;
    pkg_tui_render();
}

static void pkg_tui_render_list(void)
{
    solar_os_tui_t *tui = &pkg_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    const size_t count = pkg_tui_visible_count();
    const size_t visible = solar_os_tui_screen_content_rows(tui, 1U, 1U);

    solar_os_tui_clear(tui);
    char title_detail[32];
    if (pkg_tui.busy) {
        uint32_t bytes;
        uint32_t total;
        portENTER_CRITICAL(&pkg_tui_lock);
        bytes = pkg_tui.progress_bytes;
        total = pkg_tui.progress_total;
        portEXIT_CRITICAL(&pkg_tui_lock);
        pkg_tui.rendered_progress_bytes = bytes;
        pkg_tui.rendered_progress_total = total;
        if (total > 0U) {
            snprintf(title_detail,
                     sizeof(title_detail),
                     "%u/%u KiB",
                     (unsigned)(bytes / 1024U),
                     (unsigned)((total + 1023U) / 1024U));
        } else {
            strlcpy(title_detail, "working", sizeof(title_detail));
        }
    } else if (pkg_tui.catalog != NULL) {
        snprintf(title_detail, sizeof(title_detail), "%u signed", (unsigned)pkg_tui.catalog->count);
    } else {
        strlcpy(title_detail, "catalog unavailable", sizeof(title_detail));
    }
    solar_os_tui_draw_title(tui, "Packages", title_detail);

    solar_os_tui_viewport_reconcile(&pkg_tui.viewport, count, visible);
    for (size_t row = 0U; row < visible; row++) {
        const size_t index = pkg_tui.viewport.top + row;
        pkg_tui_node_t node;
        if (!pkg_tui_node_at(index, &node)) {
            break;
        }
        char line[96];
        if (node.is_folder) {
            snprintf(line, sizeof(line), "%c %s (%u)",
                pkg_tui.collapsed_folders & (1U << node.folder) ? '+' : '-',
                pkg_folders[node.folder].title, (unsigned)pkg_tui_folder_count(node.folder));
        } else {
            const solar_os_module_package_t *package = &pkg_tui.catalog->packages[node.package];
            snprintf(line, sizeof(line), "  [%c] %s %s",
                package->installed ? 'I' : (package->compatible ? ' ' : '!'), package->id, package->version);
        }
        solar_os_tui_write_cell(tui,
                                1U + row,
                                0U,
                                cols,
                                line,
                                index == pkg_tui.viewport.cursor ?
                                    SOLAR_OS_TUI_ATTR_INVERSE :
                                    SOLAR_OS_TUI_ATTR_NORMAL);
    }
    const char *help = pkg_tui.busy ? "Esc cancel" :
        "Arrows tree  Enter open  I install  U remove  R refresh  Q exit";
    solar_os_tui_draw_footer(tui, pkg_tui.status, help);
}

static void pkg_tui_render_details(void)
{
    solar_os_module_package_t package;
    if (!pkg_tui_selected(&package)) {
        pkg_tui.details = false;
        pkg_tui_render_list();
        return;
    }
    solar_os_tui_t *tui = &pkg_tui.tui;
    const size_t cols = solar_os_tui_cols(tui);
    solar_os_tui_clear(tui);
    solar_os_tui_draw_title(tui, package.id, package.version);

    char lines[7][160];
    snprintf(lines[0], sizeof(lines[0]), "Name: %s", package.name);
    snprintf(lines[1], sizeof(lines[1]), "Type: %s", solar_os_module_type_name(package.type));
    snprintf(lines[2], sizeof(lines[2]), "State: %s", package.installed ? "installed" : "not installed");
    snprintf(lines[3], sizeof(lines[3]), "Lifecycle ABI: %u", (unsigned)package.lifecycle_abi);
    snprintf(lines[4], sizeof(lines[4]), "Size: %u bytes", (unsigned)package.artifact_size);
    snprintf(lines[5], sizeof(lines[5]), "Description: %s", package.description);
    snprintf(lines[6],
             sizeof(lines[6]),
             "Compatibility: %s",
             package.compatible ? "compatible" : package.incompatibility);
    const size_t visible = solar_os_tui_screen_content_rows(tui, 1U, 1U);
    for (size_t row = 0U; row < visible && row < 7U; row++) {
        solar_os_tui_write_cell(tui,
                                1U + row,
                                0U,
                                cols,
                                lines[row],
                                SOLAR_OS_TUI_ATTR_NORMAL);
    }
    const char *help = pkg_tui.busy ? "Esc cancel" :
        "I install  U remove  R refresh  esc back";
    solar_os_tui_draw_footer(tui, pkg_tui.status, help);
}

static void pkg_tui_render_modal(void)
{
    const solar_os_tui_rect_t bounds = {
        .row = 1U, .col = 0U,
        .height = solar_os_tui_screen_content_rows(&pkg_tui.tui, 1U, 1U),
        .width = solar_os_tui_cols(&pkg_tui.tui),
    };
    char message[256];
    if (pkg_tui.busy) {
        uint32_t bytes, total;
        portENTER_CRITICAL(&pkg_tui_lock);
        bytes = pkg_tui.progress_bytes; total = pkg_tui.progress_total;
        portEXIT_CRITICAL(&pkg_tui_lock);
        pkg_tui.rendered_progress_bytes = bytes; pkg_tui.rendered_progress_total = total;
        const char *stage = pkg_tui.operation == PKG_TUI_OPERATION_REFRESH ? "Checking signed catalog" :
            pkg_tui.operation == PKG_TUI_OPERATION_REMOVE ? "Removing module" :
            total && bytes >= total ? "Verifying and installing" : "Downloading module";
        snprintf(message, sizeof(message), "%s%s%s\n%s\n ", stage,
            pkg_tui.operation == PKG_TUI_OPERATION_REFRESH ? "" : ": ",
            pkg_tui.operation == PKG_TUI_OPERATION_REFRESH ? "" : pkg_tui.operation_package.id,
            pkg_tui.stop_requested ? "Cancelling; please wait." : "Esc cancel");
        solar_os_tui_rect_t popup = {0};
        solar_os_tui_text_popup(&pkg_tui.tui, &bounds, "Working", message, &popup);
        if (popup.height > 2U && popup.width > 4U)
            solar_os_tui_progress_bar(&pkg_tui.tui, popup.row + popup.height - 2U,
                popup.col + 2U, popup.width - 4U, stage, bytes, total, total > 0);
    } else if (pkg_tui.prompt_operation != PKG_TUI_OPERATION_NONE) {
        bool installing = pkg_tui.prompt_operation == PKG_TUI_OPERATION_INSTALL;
        snprintf(message, sizeof(message), "%s %s %s?\ny/N",
            installing ? (pkg_tui.operation_package.installed ? "Replace" : "Install") : "Remove",
            pkg_tui.operation_package.id, pkg_tui.operation_package.version);
        solar_os_tui_text_popup(&pkg_tui.tui, &bounds,
            installing ? "Install module" : "Remove module", message, NULL);
    } else if (pkg_tui.result_popup) {
        snprintf(message, sizeof(message), "%s\nEnter or Esc to close", pkg_tui.status);
        solar_os_tui_text_popup(&pkg_tui.tui, &bounds,
            pkg_tui.operation_result == ESP_OK ? "Success" : pkg_tui.stop_requested ? "Cancelled" : "Failed",
            message, NULL);
    }
}

static void pkg_tui_render(void)
{
    if (!pkg_tui.tui_active) {
        return;
    }
    if (pkg_tui.details) {
        pkg_tui_render_details();
    } else {
        pkg_tui_render_list();
    }
    pkg_tui_render_modal();
    solar_os_tui_set_cursor_visible(&pkg_tui.tui, false);
    solar_os_tui_refresh(&pkg_tui.tui);
}

static void pkg_tui_install_selected(void)
{
    solar_os_module_package_t package;
    if (!pkg_tui_selected(&package)) {
        return;
    }
    if (!package.compatible) {
        strlcpy(pkg_tui.status, package.incompatibility, sizeof(pkg_tui.status));
        pkg_tui.stop_requested = false;
        pkg_tui.operation_result = ESP_ERR_NOT_SUPPORTED;
        pkg_tui.result_popup = true;
        return;
    }
    pkg_tui.operation_package = package;
    pkg_tui.prompt_operation = PKG_TUI_OPERATION_INSTALL;
}

static void pkg_tui_remove_selected(void)
{
    solar_os_module_package_t package;
    if (!pkg_tui_selected(&package)) {
        return;
    }
    if (!package.installed) {
        strlcpy(pkg_tui.status, "module is not installed", sizeof(pkg_tui.status));
        pkg_tui.stop_requested = false;
        pkg_tui.operation_result = ESP_ERR_NOT_FOUND;
        pkg_tui.result_popup = true;
        return;
    }
    pkg_tui.operation_package = package;
    pkg_tui.prompt_operation = PKG_TUI_OPERATION_REMOVE;
}

static esp_err_t pkg_tui_start(solar_os_context_t *ctx)
{
    memset(&pkg_tui, 0, sizeof(pkg_tui));
    pkg_tui.ctx = ctx;
    const esp_err_t err = solar_os_tui_screen_begin(&pkg_tui.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }
    pkg_tui.tui_active = true;
    const esp_err_t cached = solar_os_module_catalog_load(&pkg_tui.catalog, pkg_tui.detail, sizeof(pkg_tui.detail));
    pkg_tui_open_first_folder();
    strlcpy(pkg_tui.status, cached == ESP_OK ? "saved signed catalog loaded; R refresh" :
        "no usable saved catalog; R refresh", sizeof(pkg_tui.status));
    pkg_tui_render();
    return ESP_OK;
}

static void pkg_tui_suspend(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&pkg_tui.tui, true);
    solar_os_tui_refresh(&pkg_tui.tui);
}

static void pkg_tui_resume(solar_os_context_t *ctx)
{
    pkg_tui.ctx = ctx;
    pkg_tui_finish_operation();
    pkg_tui_render();
}

static void pkg_tui_cleanup_screen(void)
{
    if (pkg_tui.tui_active) {
        solar_os_tui_set_cursor_visible(&pkg_tui.tui, true);
        solar_os_tui_clear(&pkg_tui.tui);
        solar_os_tui_refresh(&pkg_tui.tui);
        solar_os_tui_end(&pkg_tui.tui);
        pkg_tui.tui_active = false;
    }
}

static void pkg_tui_cleanup(void)
{
    pkg_tui_cleanup_screen();
    solar_os_module_catalog_free(pkg_tui.catalog);
    pkg_tui.catalog = NULL;
    solar_os_module_catalog_free(pkg_tui.operation_catalog);
    pkg_tui.operation_catalog = NULL;
}

static void pkg_tui_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    pkg_tui.stop_requested = true;
    if (pkg_tui.busy &&
        !solar_os_task_wait_done(pkg_tui.task,
                                 &pkg_tui.task_done,
                                 SOLAR_OS_TASK_STOP_WAIT_MS)) {
        pkg_tui_cleanup_screen();
        return;
    }
    pkg_tui_cleanup();
}

static bool pkg_tui_state_release_ready(void)
{
    return pkg_tui.task == NULL || pkg_tui.task_done;
}

static bool pkg_tui_event(solar_os_context_t *ctx,
                          const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        pkg_tui_finish_operation();
        if (pkg_tui.busy) {
            uint32_t bytes;
            uint32_t total;
            portENTER_CRITICAL(&pkg_tui_lock);
            bytes = pkg_tui.progress_bytes;
            total = pkg_tui.progress_total;
            portEXIT_CRITICAL(&pkg_tui_lock);
            if (bytes != pkg_tui.rendered_progress_bytes ||
                total != pkg_tui.rendered_progress_total) {
                pkg_tui_render();
            }
        }
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }

    const uint8_t key = (uint8_t)event->data.ch;
    if (key == SOLAR_OS_KEY_APP_EXIT) {
        pkg_tui.stop_requested = true;
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (pkg_tui.prompt_operation != PKG_TUI_OPERATION_NONE) {
        if (key == 'y' || key == 'Y')
            (void)pkg_tui_start_operation(pkg_tui.prompt_operation, &pkg_tui.operation_package);
        else if (key == 'n' || key == 'N' || key == SOLAR_OS_KEY_ESCAPE || key == '\r' || key == '\n')
            pkg_tui.prompt_operation = PKG_TUI_OPERATION_NONE;
        pkg_tui_render();
        return true;
    }
    if (pkg_tui.result_popup) {
        if (key == SOLAR_OS_KEY_ESCAPE || key == '\r' || key == '\n' || key == 'q' || key == 'Q')
            pkg_tui.result_popup = false;
        pkg_tui_render();
        return true;
    }
    if (pkg_tui.busy) {
        if (key == SOLAR_OS_KEY_ESCAPE || key == 0x03U) {
            pkg_tui.stop_requested = true;
            strlcpy(pkg_tui.status, "cancelling...", sizeof(pkg_tui.status));
            pkg_tui_render();
        }
        return true;
    }
    if (key == SOLAR_OS_KEY_APP_EXIT ||
        (!pkg_tui.details &&
         (key == SOLAR_OS_KEY_ESCAPE || key == 'q' || key == 'Q'))) {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }
    if (pkg_tui.details &&
        (key == SOLAR_OS_KEY_ESCAPE || key == SOLAR_OS_KEY_LEFT ||
         key == 'q' || key == 'Q')) {
        pkg_tui.details = false;
        pkg_tui.status[0] = '\0';
    } else if (key == 'r' || key == 'R') {
        (void)pkg_tui_start_operation(PKG_TUI_OPERATION_REFRESH, NULL);
    } else if (key == 'i' || key == 'I') {
        pkg_tui_install_selected();
    } else if (key == 'u' || key == 'U') {
        pkg_tui_remove_selected();
    } else if (!pkg_tui.details &&
               (key == SOLAR_OS_KEY_ENTER || key == '\r' ||
                key == '\n' || key == SOLAR_OS_KEY_RIGHT)) {
        pkg_tui_node_t node;
        if (pkg_tui_node_at(pkg_tui.viewport.cursor, &node)) {
            if (node.is_folder) {
                if (key == SOLAR_OS_KEY_RIGHT) pkg_tui.collapsed_folders &= ~(1U << node.folder);
                else pkg_tui.collapsed_folders ^= 1U << node.folder;
            } else pkg_tui.details = true;
        }
    } else if (!pkg_tui.details && key == SOLAR_OS_KEY_LEFT) {
        pkg_tui_tree_left();
    } else if (!pkg_tui.details &&
               solar_os_tui_viewport_key(&pkg_tui.viewport,
                                         key,
                                         pkg_tui_visible_count(),
                                         solar_os_tui_screen_content_rows(
                                             &pkg_tui.tui, 1U, 1U),
                                         false)) {
        pkg_tui.status[0] = '\0';
    }
    pkg_tui_render();
    return true;
}

static void pkg_tui_title(solar_os_context_t *ctx,
                          char *buffer,
                          size_t buffer_len)
{
    (void)ctx;
    strlcpy(buffer, "Packages", buffer_len);
}

static const solar_os_app_t pkg_tui_app = {
    .name = "pkg",
    .summary = "native module package manager",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = pkg_tui_start,
    .suspend = pkg_tui_suspend,
    .resume = pkg_tui_resume,
    .stop = pkg_tui_stop,
    .event = pkg_tui_event,
    .title = pkg_tui_title,
    .state_slot = &pkg_tui_state,
    .state_size = sizeof(pkg_tui_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
    .state_release_ready = pkg_tui_state_release_ready,
    .state_release_cleanup = pkg_tui_cleanup,
    .worker_stack_bytes = PKG_TUI_TASK_STACK,
};

esp_err_t solar_os_shell_launch_pkg_tui(solar_os_context_t *ctx)
{
    return solar_os_context_request_launch(ctx, &pkg_tui_app, 0, NULL);
}
