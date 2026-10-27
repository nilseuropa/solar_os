#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define SOLAR_OS_FTP_NAME_MAX 64
#define SOLAR_OS_STORAGE_PATH_MAX 128
static struct { int tui; bool show_hidden; } ftp_app;
static size_t rows = 20;
static bool fullscreen;
static size_t solar_os_tui_rows(const int *tui) { (void)tui; return rows; }
static bool solar_os_tui_screen_fullscreen(const int *tui) { (void)tui; return fullscreen; }

/* NAVIGATION_SOURCE */

int main(void)
{
    ftp_app_entry_t entries[] = {
        {.name = "..", .is_directory = true, .parent = true},
        {.name = ".config", .is_directory = true},
        {.name = "docs", .is_directory = true},
        {.name = ".secret"},
        {.name = "readme"},
    };
    ftp_app_pane_t pane = {.entries = entries, .listed_count = 5, .count = 5, .cursor = 4, .top = 4};
    for (unsigned i = 0; i < 5; i++) {
        ftp_app.show_hidden = false;
        ftp_app_filter_hidden(&pane);
        assert(pane.count == 3 && pane.listed_count == 5);
        assert(pane.cursor < pane.count && pane.top < pane.count);
        assert(!strcmp(pane.entries[0].name, ".."));
        assert(!strcmp(pane.entries[1].name, "docs"));
        assert(!strcmp(pane.entries[2].name, "readme"));
        ftp_app.show_hidden = true;
        ftp_app_filter_hidden(&pane);
        assert(pane.count == 5);
        assert(!strcmp(pane.entries[1].name, ".config"));
        assert(!strcmp(pane.entries[3].name, ".secret"));
    }
    pane = (ftp_app_pane_t){.count = 40};
    ftp_app_page(&pane, true);
    assert(pane.cursor == 15);
    ftp_app_page(&pane, false);
    assert(pane.cursor == 0);
    fullscreen = true;
    ftp_app_page(&pane, true);
    assert(pane.cursor == 17);
    ftp_app_page(&pane, true);
    ftp_app_page(&pane, true);
    assert(pane.cursor == 39);
    ftp_app_page(&pane, false);
    assert(pane.cursor == 22);
    rows = 2;
    ftp_app_page(&pane, true);
    assert(pane.cursor == 23);
    pane = (ftp_app_pane_t){0};
    ftp_app_page(&pane, true);
    ftp_app_page(&pane, false);
    assert(pane.cursor == 0);
    ftp_app.show_hidden = false;
    pane = (ftp_app_pane_t){.entries = entries + 3, .listed_count = 1, .count = 1};
    /* After sorting above, index 3 is the hidden file. */
    ftp_app_filter_hidden(&pane);
    assert(pane.count == 0 && pane.cursor == 0 && pane.top == 0);
    ftp_app.show_hidden = true;
    ftp_app_filter_hidden(&pane);
    assert(pane.count == 1);
    return 0;
}
