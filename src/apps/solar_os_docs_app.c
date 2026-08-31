#include "solar_os_docs_app.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
#include "solar_os_docs.h"
#endif
#include "solar_os_keys.h"
#include "solar_os_less.h"
#include "solar_os_manual.h"
#if SOLAR_OS_PACKAGE_APP_READER
#include "solar_os_reader.h"
#endif
#include "solar_os_shell_io.h"
#include "solar_os_terminal.h"
#include "solar_os_tui.h"
#include "solar_os_tui_widgets.h"

#define DOCS_APP_LINE_MAX (SOLAR_OS_TERMINAL_MAX_COLS * 4U + 1U)
#define DOCS_APP_MAX_FOLDABLE_SECTIONS 64U

typedef struct {
    solar_os_tui_t tui;
    size_t cursor;
    size_t top;
    uint64_t collapsed_sections;
} docs_app_state_t;

typedef struct {
    size_t index;
    size_t first_page;
    size_t page_count;
    const char *title;
} docs_section_info_t;

typedef enum {
    DOCS_NODE_SECTION,
    DOCS_NODE_PAGE,
} docs_node_kind_t;

typedef struct {
    docs_node_kind_t kind;
    docs_section_info_t section;
    const solar_os_manual_page_t *page;
} docs_node_t;

static void *docs_app_state;
#define docs_app (*(docs_app_state_t *)docs_app_state)

static size_t docs_app_visible_rows(void)
{
    const size_t rows = solar_os_tui_rows(&docs_app.tui);
    return rows > 2U ? rows - 2U : 0U;
}

static bool docs_section_info(size_t wanted, docs_section_info_t *info)
{
    const size_t page_count = solar_os_manual_count();
    size_t section_index = SIZE_MAX;
    const char *previous = NULL;

    if (info == NULL) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    for (size_t page_index = 0U; page_index < page_count; page_index++) {
        const solar_os_manual_page_t *page =
            solar_os_manual_get(page_index);
        if (page == NULL) {
            continue;
        }
        if (previous == NULL || strcmp(previous, page->section) != 0) {
            section_index++;
            previous = page->section;
            if (section_index > wanted) {
                break;
            }
            if (section_index == wanted) {
                info->index = wanted;
                info->first_page = page_index;
                info->title = page->section_title;
            }
        }
        if (section_index == wanted) {
            info->page_count++;
        }
    }
    return info->page_count > 0U;
}

static size_t docs_section_count(void)
{
    size_t count = 0U;
    docs_section_info_t section;
    while (docs_section_info(count, &section)) {
        count++;
    }
    return count;
}

static bool docs_section_collapsed(size_t section_index)
{
    if (section_index >= DOCS_APP_MAX_FOLDABLE_SECTIONS) {
        return false;
    }
    return (docs_app.collapsed_sections & (UINT64_C(1) << section_index)) != 0U;
}

static void docs_set_section_collapsed(size_t section_index, bool collapsed)
{
    if (section_index >= DOCS_APP_MAX_FOLDABLE_SECTIONS) {
        return;
    }
    const uint64_t bit = UINT64_C(1) << section_index;
    if (collapsed) {
        docs_app.collapsed_sections |= bit;
    } else {
        docs_app.collapsed_sections &= ~bit;
    }
}

static size_t docs_visible_count(void)
{
    size_t visible = 0U;
    docs_section_info_t section;
    for (size_t index = 0U; docs_section_info(index, &section); index++) {
        visible++;
        if (!docs_section_collapsed(index)) {
            visible += section.page_count;
        }
    }
    return visible;
}

static bool docs_node_at(size_t visible_index, docs_node_t *node)
{
    docs_section_info_t section;
    if (node == NULL) {
        return false;
    }
    for (size_t index = 0U; docs_section_info(index, &section); index++) {
        if (visible_index == 0U) {
            memset(node, 0, sizeof(*node));
            node->kind = DOCS_NODE_SECTION;
            node->section = section;
            return true;
        }
        visible_index--;
        if (docs_section_collapsed(index)) {
            continue;
        }
        if (visible_index < section.page_count) {
            const size_t page_index =
                section.first_page + visible_index;
            const solar_os_manual_page_t *page =
                solar_os_manual_get(page_index);
            if (page == NULL) {
                return false;
            }
            memset(node, 0, sizeof(*node));
            node->kind = DOCS_NODE_PAGE;
            node->section = section;
            node->page = page;
            return true;
        }
        visible_index -= section.page_count;
    }
    return false;
}

static size_t docs_section_visible_index(size_t wanted)
{
    size_t visible = 0U;
    docs_section_info_t section;
    for (size_t index = 0U; docs_section_info(index, &section); index++) {
        if (index == wanted) {
            return visible;
        }
        visible++;
        if (!docs_section_collapsed(index)) {
            visible += section.page_count;
        }
    }
    return 0U;
}

static bool docs_page_location(size_t page_index,
                               size_t *section_index,
                               size_t *offset)
{
    docs_section_info_t section;
    for (size_t index = 0U; docs_section_info(index, &section); index++) {
        if (page_index >= section.first_page &&
            page_index < section.first_page + section.page_count) {
            if (section_index != NULL) {
                *section_index = index;
            }
            if (offset != NULL) {
                *offset = page_index - section.first_page;
            }
            return true;
        }
    }
    return false;
}

static void docs_app_ensure_visible(void)
{
    const size_t count = docs_visible_count();
    const size_t visible = docs_app_visible_rows();
    if (count == 0U || visible == 0U) {
        docs_app.cursor = 0U;
        docs_app.top = 0U;
        return;
    }
    if (docs_app.cursor >= count) {
        docs_app.cursor = count - 1U;
    }
    if (docs_app.cursor < docs_app.top) {
        docs_app.top = docs_app.cursor;
    } else if (docs_app.cursor >= docs_app.top + visible) {
        docs_app.top = docs_app.cursor - visible + 1U;
    }
}

static void docs_app_header(char *line, size_t line_len)
{
    const size_t count = solar_os_manual_count();
#if SOLAR_OS_PACKAGE_SERVICE_DOCS
    solar_os_docs_status_t status;
    if (solar_os_docs_get_status(&status) == ESP_OK && status.available) {
        snprintf(line,
                 line_len,
                 "SolarOS manual  %u topics  downloaded %s",
                 (unsigned)count,
                 status.revision);
        return;
    }
#endif
    snprintf(line,
             line_len,
             "SolarOS manual  %u topics  embedded",
             (unsigned)count);
}

static void docs_app_render(void)
{
    const size_t rows = solar_os_tui_rows(&docs_app.tui);
    const size_t cols = solar_os_tui_cols(&docs_app.tui);
    const size_t visible = docs_app_visible_rows();
    char line[DOCS_APP_LINE_MAX];

    docs_app_ensure_visible();
    solar_os_tui_set_cursor_visible(&docs_app.tui, false);
    solar_os_tui_clear(&docs_app.tui);
    docs_app_header(line, sizeof(line));
    solar_os_tui_draw_title(&docs_app.tui, line, NULL);

    if (rows < 3U || cols < 12U) {
        solar_os_tui_draw_too_small(&docs_app.tui, "docs");
    } else {
        for (size_t row = 0U; row < visible; row++) {
            const size_t visible_index = docs_app.top + row;
            docs_node_t node;
            if (!docs_node_at(visible_index, &node)) {
                solar_os_tui_write_cell(&docs_app.tui, row + 1U,
                                    0U,
                                    cols,
                                    "",
                                    SOLAR_OS_TUI_ATTR_NORMAL);
                continue;
            }
            if (node.kind == DOCS_NODE_SECTION) {
                snprintf(line,
                         sizeof(line),
                         "[%c] %s (%u)",
                         docs_section_collapsed(node.section.index) ? '+' : '-',
                         node.section.title,
                         (unsigned)node.section.page_count);
            } else if (cols >= 54U) {
                snprintf(line,
                         sizeof(line),
                         "  |-- %-25.25s %s",
                         node.page->title,
                         node.page->summary);
            } else {
                snprintf(line,
                         sizeof(line),
                         "  |-- %s",
                         node.page->title);
            }
            const uint8_t attr = visible_index == docs_app.cursor ?
                SOLAR_OS_TUI_ATTR_INVERSE | SOLAR_OS_TUI_ATTR_BOLD :
                (node.kind == DOCS_NODE_SECTION ?
                    SOLAR_OS_TUI_ATTR_BOLD :
                    SOLAR_OS_TUI_ATTR_NORMAL);
            solar_os_tui_write_cell(&docs_app.tui, row + 1U, 0U, cols, line, attr);
        }
    }

    if (rows > 0U) {
        solar_os_tui_draw_help(&docs_app.tui,
                               "Enter open/fold  Left/Right tree  q quit");
    }
    solar_os_tui_refresh(&docs_app.tui);
}

static bool docs_use_graphic_reader(solar_os_context_t *ctx)
{
#if SOLAR_OS_PACKAGE_APP_READER
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    const bool local_display =
        (io != NULL && solar_os_shell_io_terminal(io) != NULL) ||
        (io == NULL && solar_os_context_terminal(ctx) != NULL);
    return local_display && solar_os_context_gfx(ctx) != NULL;
#else
    (void)ctx;
    return false;
#endif
}

static bool docs_app_open_selected(solar_os_context_t *ctx)
{
    docs_node_t node;
    if (!docs_node_at(docs_app.cursor, &node) ||
        node.kind != DOCS_NODE_PAGE ||
        node.page == NULL) {
        return false;
    }

    const bool use_reader = docs_use_graphic_reader(ctx);
    char app_arg[SOLAR_OS_APP_ARG_LEN];
    char source_arg[SOLAR_OS_APP_ARG_LEN];
    char *argv[] = {app_arg, source_arg};
#if SOLAR_OS_PACKAGE_APP_READER
    const solar_os_app_t *viewer =
        use_reader ? &solar_os_reader_app : &solar_os_less_app;
#else
    const solar_os_app_t *viewer = &solar_os_less_app;
#endif
    strlcpy(app_arg, use_reader ? "reader" : "less", sizeof(app_arg));
    const int written =
        snprintf(source_arg, sizeof(source_arg), "man:%s", node.page->id);
    if (written < 0 || (size_t)written >= sizeof(source_arg)) {
        return false;
    }
    return solar_os_context_request_launch_ex(ctx,
                                              viewer,
                                              2,
                                              argv,
                                              SOLAR_OS_LAUNCH_CHILD_RETURN) == ESP_OK;
}

static void docs_app_move(int delta)
{
    const size_t count = docs_visible_count();
    if (count == 0U) {
        return;
    }
    if (delta < 0 && docs_app.cursor > 0U) {
        docs_app.cursor--;
    } else if (delta > 0 && docs_app.cursor + 1U < count) {
        docs_app.cursor++;
    }
}

static void docs_app_page(bool down)
{
    const size_t count = docs_visible_count();
    const size_t visible = docs_app_visible_rows();
    const size_t step = visible > 1U ? visible - 1U : 1U;
    if (count == 0U) {
        return;
    }
    if (down) {
        docs_app.cursor = docs_app.cursor + step < count ?
            docs_app.cursor + step : count - 1U;
    } else {
        docs_app.cursor = docs_app.cursor > step ?
            docs_app.cursor - step : 0U;
    }
}

static void docs_toggle_section(const docs_node_t *node, bool expand)
{
    if (node == NULL || node->kind != DOCS_NODE_SECTION) {
        return;
    }
    docs_set_section_collapsed(node->section.index, !expand);
}

static esp_err_t docs_app_start(solar_os_context_t *ctx)
{
    memset(&docs_app, 0, sizeof(docs_app));
    const esp_err_t err = solar_os_tui_screen_begin(&docs_app.tui, ctx);
    if (err != ESP_OK) {
        return err;
    }

    docs_app.collapsed_sections = UINT64_MAX;

    const char *requested = solar_os_context_argc(ctx) >= 2 ?
        solar_os_context_argv(ctx, 1) : NULL;
    const solar_os_manual_page_t *page =
        requested != NULL ? solar_os_manual_find(requested) : NULL;
    if (page != NULL) {
        for (size_t page_index = 0U;
             page_index < solar_os_manual_count();
             page_index++) {
            if (solar_os_manual_get(page_index) != page) {
                continue;
            }
            size_t section_index = 0U;
            size_t section_offset = 0U;
            if (docs_page_location(page_index,
                                   &section_index,
                                   &section_offset)) {
                docs_set_section_collapsed(section_index, false);
                docs_app.cursor =
                    docs_section_visible_index(section_index) +
                    1U +
                    section_offset;
            }
            break;
        }
    }
    docs_app_render();
    return ESP_OK;
}

static void docs_app_stop(solar_os_context_t *ctx)
{
    (void)ctx;
    solar_os_tui_set_cursor_visible(&docs_app.tui, true);
    solar_os_tui_refresh(&docs_app.tui);
    solar_os_tui_end(&docs_app.tui);
    memset(&docs_app, 0, sizeof(docs_app));
}

static void docs_app_resume(solar_os_context_t *ctx)
{
    (void)ctx;
    docs_app_render();
}

static void docs_app_title(solar_os_context_t *ctx,
                           char *buffer,
                           size_t buffer_len)
{
    (void)ctx;
    if (buffer != NULL && buffer_len > 0U) {
        strlcpy(buffer, "help", buffer_len);
    }
}

static bool docs_app_event(solar_os_context_t *ctx,
                           const solar_os_event_t *event)
{
    if (event == NULL || event->type != SOLAR_OS_EVENT_CHAR) {
        return true;
    }
    const uint8_t ch = (uint8_t)event->data.ch;
    if (ch == SOLAR_OS_KEY_APP_EXIT || ch == SOLAR_OS_KEY_ESCAPE ||
        ch == 'q' || ch == 'Q') {
        solar_os_context_finish(ctx, 0, NULL);
        return true;
    }

    docs_node_t node;
    const bool selected = docs_node_at(docs_app.cursor, &node);
    switch (ch) {
    case SOLAR_OS_KEY_UP:
    case 'k':
        docs_app_move(-1);
        break;
    case SOLAR_OS_KEY_DOWN:
    case 'j':
        docs_app_move(1);
        break;
    case SOLAR_OS_KEY_PAGE_UP:
        docs_app_page(false);
        break;
    case SOLAR_OS_KEY_PAGE_DOWN:
        docs_app_page(true);
        break;
    case SOLAR_OS_KEY_HOME:
        docs_app.cursor = 0U;
        break;
    case SOLAR_OS_KEY_END:
        if (docs_visible_count() > 0U) {
            docs_app.cursor = docs_visible_count() - 1U;
        }
        break;
    case SOLAR_OS_KEY_LEFT:
        if (selected && node.kind == DOCS_NODE_SECTION) {
            docs_toggle_section(&node, false);
        } else if (selected) {
            docs_app.cursor =
                docs_section_visible_index(node.section.index);
        }
        break;
    case SOLAR_OS_KEY_RIGHT:
        if (selected && node.kind == DOCS_NODE_SECTION) {
            docs_toggle_section(&node, true);
        } else {
            (void)docs_app_open_selected(ctx);
            return true;
        }
        break;
    case '\r':
    case '\n':
    case ' ':
        if (selected && node.kind == DOCS_NODE_SECTION) {
            docs_toggle_section(
                &node,
                docs_section_collapsed(node.section.index));
        } else {
            (void)docs_app_open_selected(ctx);
            return true;
        }
        break;
    default:
        return true;
    }
    docs_app_render();
    return true;
}

const solar_os_app_t solar_os_docs_app = {
    .name = "help",
    .summary = "browse the SolarOS manual",
    .app_class = SOLAR_OS_APP_CLASS_TUI,
    .flags = SOLAR_OS_APP_FLAG_RESUMABLE,
    .start = docs_app_start,
    .resume = docs_app_resume,
    .stop = docs_app_stop,
    .event = docs_app_event,
    .title = docs_app_title,
    .state_slot = &docs_app_state,
    .state_size = sizeof(docs_app_state_t),
    .state_storage = SOLAR_OS_APP_STATE_TRANSIENT,
};
