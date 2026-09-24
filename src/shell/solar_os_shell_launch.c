#include "solar_os_shell_launch.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

static bool app_has_first_path_arg(const char *app_name)
{
    return strcmp(app_name, "edit") == 0 ||
        strcmp(app_name, "hexedit") == 0 ||
        strcmp(app_name, "notes") == 0 ||
        strcmp(app_name, "writer") == 0 ||
        strcmp(app_name, "sheet") == 0 ||
        strcmp(app_name, "gameboy") == 0 ||
        strcmp(app_name, "launcher") == 0 ||
        strcmp(app_name, "python") == 0 ||
        strcmp(app_name, "lua") == 0;
}

static int mixed_ui_path_arg(int argc, char *const argv[])
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--tui") == 0) {
            continue;
        }
        return argv[i][0] == '-' ? -1 : i;
    }
    return -1;
}

static int option_path_arg(int argc,
                           char *const argv[],
                           const char *short_option,
                           const char *long_option)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], short_option) == 0 ||
            (long_option != NULL && strcmp(argv[i], long_option) == 0)) {
            return i + 1;
        }
    }
    return -1;
}

static int audio_path_arg(const char *app_name, int argc, char *const argv[])
{
    const bool playback = strcmp(app_name, "aplay") == 0;
    for (int i = 1; i < argc; i++) {
        if ((playback && strcmp(argv[i], "-v") == 0) ||
            (!playback && (strcmp(argv[i], "-d") == 0 ||
                           strcmp(argv[i], "-i") == 0))) {
            i++;
            continue;
        }
        if (argv[i][0] != '-') {
            return i;
        }
    }
    return -1;
}

int solar_os_shell_launch_path_arg(const char *app_name,
                                   int argc,
                                   char *const argv[])
{
    if (app_name == NULL || argc < 2 || argv == NULL) {
        return -1;
    }

    if (app_has_first_path_arg(app_name)) {
        return 1;
    }
    if (strcmp(app_name, "player") == 0 ||
        strcmp(app_name, "recorder") == 0) {
        return mixed_ui_path_arg(argc, argv);
    }
    if (strcmp(app_name, "files") == 0) {
        return strcmp(argv[1], "--launcher") == 0 ?
            (argc >= 3 ? 2 : -1) : 1;
    }
    if (strcmp(app_name, "less") == 0) {
        return strncmp(argv[1], "man:", 4U) == 0 ? -1 : 1;
    }
    if (strcmp(app_name, "reader") == 0) {
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--pager") == 0) {
                continue;
            }
            return strncmp(argv[i], "man:", 4U) == 0 ? -1 : i;
        }
        return -1;
    }
    if (strcmp(app_name, "plot") == 0) {
        return option_path_arg(argc, argv, "-f", "--file");
    }
    if (strcmp(app_name, "curl") == 0) {
        return option_path_arg(argc, argv, "-o", NULL);
    }
    if (strcmp(app_name, "aplay") == 0 || strcmp(app_name, "arecord") == 0) {
        return audio_path_arg(app_name, argc, argv);
    }
    if (strcmp(app_name, "view") == 0) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                return i;
            }
        }
        return -1;
    }
    if (strcmp(app_name, "agent") == 0 &&
        argc >= 4 &&
        strcmp(argv[1], "script") == 0 &&
        strcmp(argv[3], "-c") != 0) {
        return 3;
    }

    return -1;
}

bool solar_os_shell_launch_raw_remainder(
    const char *line,
    int prefix_argc,
    const char *const prefix_argv[],
    char *buffer,
    size_t buffer_len)
{
    if (line == NULL || prefix_argc <= 0 || prefix_argv == NULL ||
        buffer == NULL || buffer_len == 0U) {
        return false;
    }

    const char *cursor = line;
    for (int i = 0; i < prefix_argc; i++) {
        while (isspace((unsigned char)*cursor)) {
            cursor++;
        }
        const char *start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) {
            cursor++;
        }
        const size_t length = (size_t)(cursor - start);
        if (prefix_argv[i] == NULL || strlen(prefix_argv[i]) != length ||
            strncmp(start, prefix_argv[i], length) != 0) {
            return false;
        }
    }

    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    const char *end = cursor + strlen(cursor);
    while (end > cursor && isspace((unsigned char)end[-1])) {
        end--;
    }
    if (end == cursor) {
        return false;
    }

    if (end - cursor >= 2 &&
        (cursor[0] == '\'' || cursor[0] == '"') &&
        end[-1] == cursor[0]) {
        cursor++;
        end--;
    }

    const size_t length = (size_t)(end - cursor);
    if (length >= buffer_len) {
        return false;
    }
    memcpy(buffer, cursor, length);
    buffer[length] = '\0';
    return true;
}

bool solar_os_shell_path_is_script(const char *path)
{
    if (path == NULL) {
        return false;
    }
    const size_t len = strlen(path);
    return len > 3U &&
        path[len - 3U] == '.' &&
        tolower((unsigned char)path[len - 2U]) == 's' &&
        tolower((unsigned char)path[len - 1U]) == 'h';
}
