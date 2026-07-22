#include "solar_os_shell_commands.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_zip.h"

#define SHELL_PATH_MAX SOLAR_OS_STORAGE_PATH_MAX
#define SHELL_CAT_MAX_BYTES 4096
#define SHELL_ZIP_SOURCE_MAX 64
#define SHELL_ZIP_TASK_STACK 24576
#define SHELL_ZIP_TASK_PRIORITY 4
#define SHELL_ZIP_WAIT_POLL_MS 20U

typedef struct {
    bool show_all;
    bool human;
} shell_ls_options_t;

typedef struct {
    char dir_path[SHELL_PATH_MAX];
    char base_arg[SHELL_PATH_MAX];
    char name_pattern[SHELL_PATH_MAX];
} shell_wildcard_path_t;

typedef bool (*shell_wildcard_match_callback_t)(solar_os_context_t *ctx,
                                                const char *full_path,
                                                const char *display_path,
                                                const char *name,
                                                void *user);

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_context_io(ctx);
}

static bool shell_arg_has_wildcards(const char *arg)
{
    return arg != NULL && (strchr(arg, '*') != NULL || strchr(arg, '?') != NULL);
}

static bool shell_wildcard_match(const char *pattern, const char *text)
{
    const char *star = NULL;
    const char *retry = NULL;

    while (*text != '\0') {
        if (*pattern == '?' || *pattern == *text) {
            pattern++;
            text++;
        } else if (*pattern == '*') {
            star = pattern++;
            retry = text;
        } else if (star != NULL) {
            pattern = star + 1;
            text = ++retry;
        } else {
            return false;
        }
    }

    while (*pattern == '*') {
        pattern++;
    }
    return *pattern == '\0';
}

static const char *shell_path_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash != NULL ? slash + 1 : path;
}

static void join_path(char *out, size_t out_len, const char *dir, const char *name)
{
    const size_t dir_len = strlen(dir);

    if (strcmp(dir, "/") == 0) {
        snprintf(out, out_len, "/%s", name);
    } else if (dir_len > 0 && dir[dir_len - 1] == '/') {
        snprintf(out, out_len, "%s%s", dir, name);
    } else {
        snprintf(out, out_len, "%s/%s", dir, name);
    }
}

static bool join_path_checked(char *out, size_t out_len, const char *dir, const char *name)
{
    const size_t dir_len = strlen(dir);
    int written = 0;

    if (strcmp(dir, "/") == 0) {
        written = snprintf(out, out_len, "/%s", name);
    } else if (dir_len > 0 && dir[dir_len - 1] == '/') {
        written = snprintf(out, out_len, "%s%s", dir, name);
    } else {
        written = snprintf(out, out_len, "%s/%s", dir, name);
    }

    return written >= 0 && (size_t)written < out_len;
}

static bool shell_path_is_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return false;
    }

    closedir(dir);
    return true;
}

static bool shell_prepare_wildcard_path(solar_os_context_t *ctx,
                                        const char *arg,
                                        shell_wildcard_path_t *wildcard)
{
    char dir_arg[SHELL_PATH_MAX];
    const char *name_pattern = arg;
    const char *dir_to_resolve = NULL;
    const char *slash = strrchr(arg, '/');

    if (slash != NULL) {
        const size_t dir_len = (size_t)(slash - arg);
        const size_t base_len = dir_len + 1;
        if (base_len >= sizeof(wildcard->base_arg) ||
            strlen(slash + 1) >= sizeof(wildcard->name_pattern)) {
            return false;
        }

        memcpy(wildcard->base_arg, arg, base_len);
        wildcard->base_arg[base_len] = '\0';
        name_pattern = slash + 1;

        if (dir_len == 0) {
            strlcpy(dir_arg, "/", sizeof(dir_arg));
        } else {
            if (dir_len >= sizeof(dir_arg)) {
                return false;
            }
            memcpy(dir_arg, arg, dir_len);
            dir_arg[dir_len] = '\0';
        }

        if (shell_arg_has_wildcards(dir_arg)) {
            return false;
        }
        dir_to_resolve = dir_arg;
    } else {
        wildcard->base_arg[0] = '\0';
    }

    if (!shell_arg_has_wildcards(name_pattern)) {
        return false;
    }

    strlcpy(wildcard->name_pattern, name_pattern, sizeof(wildcard->name_pattern));
    return solar_os_shell_resolve_path(ctx,
                                       dir_to_resolve,
                                       wildcard->dir_path,
                                       sizeof(wildcard->dir_path)) == ESP_OK;
}

static size_t shell_for_each_wildcard_match(solar_os_context_t *ctx,
                                            const char *command,
                                            const char *arg,
                                            shell_wildcard_match_callback_t callback,
                                            void *user,
                                            bool *had_error)
{
    shell_wildcard_path_t wildcard;
    solar_os_shell_io_t *term = terminal(ctx);
    size_t match_count = 0;

    if (had_error != NULL) {
        *had_error = false;
    }

    if (!shell_prepare_wildcard_path(ctx, arg, &wildcard)) {
        solar_os_shell_io_printf(term,
                                 "%s: wildcards are only supported in the filename: %s\n",
                                 command,
                                 arg);
        if (had_error != NULL) {
            *had_error = true;
        }
        return 0;
    }

    DIR *dir = opendir(wildcard.dir_path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term,
                                 "%s: cannot open %s: %s\n",
                                 command,
                                 wildcard.dir_path,
                                 strerror(errno));
        if (had_error != NULL) {
            *had_error = true;
        }
        return 0;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!shell_wildcard_match(wildcard.name_pattern, entry->d_name)) {
            continue;
        }

        match_count++;

        if (callback != NULL) {
            char full_path[SHELL_PATH_MAX];
            char display_path[SHELL_PATH_MAX];
            join_path(full_path, sizeof(full_path), wildcard.dir_path, entry->d_name);
            snprintf(display_path,
                     sizeof(display_path),
                     "%s%s",
                     wildcard.base_arg,
                     entry->d_name);
            if (!callback(ctx, full_path, display_path, entry->d_name, user)) {
                break;
            }
        }
    }

    closedir(dir);
    return match_count;
}

static void shell_report_no_wildcard_matches(solar_os_shell_io_t *term,
                                             const char *command,
                                             const char *arg,
                                             size_t match_count,
                                             bool had_error)
{
    if (match_count == 0 && !had_error) {
        solar_os_shell_io_printf(term, "%s: no match: %s\n", command, arg);
    }
}

void solar_os_shell_cmd_cd(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    char path[SHELL_PATH_MAX];

    if (argc > 2) {
        solar_os_shell_io_writeln(term, "usage: cd [path]");
        return;
    }

    if (!solar_os_shell_resolve_path_for_command(ctx,
                                                 term,
                                                 "cd",
                                                 argc == 2 ? argv[1] : "/",
                                                 path,
                                                 sizeof(path))) {
        return;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term,
                                 "cd: cannot open %s: %s\n",
                                 argc == 2 ? argv[1] : "/",
                                 strerror(errno));
        return;
    }
    closedir(dir);

    (void)solar_os_shell_set_cwd(ctx, path);
}

static bool shell_ls_hidden_name(const char *name)
{
    return name != NULL && name[0] == '.';
}

static void shell_format_file_size(char *buffer, size_t buffer_len, off_t size, bool human)
{
    if (buffer == NULL || buffer_len == 0) {
        return;
    }

    if (size < 0) {
        strlcpy(buffer, "?", buffer_len);
        return;
    }

    if (!human) {
        snprintf(buffer, buffer_len, "%lld", (long long)size);
        return;
    }

    static const char units[] = {'B', 'K', 'M', 'G'};
    uint64_t scaled_x10 = (uint64_t)size * 10U;
    size_t unit = 0;
    while (scaled_x10 >= 10240U && unit + 1 < sizeof(units)) {
        scaled_x10 = (scaled_x10 + 512U) / 1024U;
        unit++;
    }

    if (unit == 0) {
        snprintf(buffer, buffer_len, "%lluB", (unsigned long long)(scaled_x10 / 10U));
    } else if (scaled_x10 < 100U) {
        snprintf(buffer,
                 buffer_len,
                 "%llu.%llu%c",
                 (unsigned long long)(scaled_x10 / 10U),
                 (unsigned long long)(scaled_x10 % 10U),
                 units[unit]);
    } else {
        snprintf(buffer,
                 buffer_len,
                 "%llu%c",
                 (unsigned long long)((scaled_x10 + 5U) / 10U),
                 units[unit]);
    }
}

static void shell_ls_print_entry_with_options(solar_os_shell_io_t *term,
                                              const char *full_path,
                                              const char *display_name,
                                              const shell_ls_options_t *options)
{
    struct stat st;
    const bool stat_ok = full_path != NULL && stat(full_path, &st) == 0;
    const bool is_dir = stat_ok ? S_ISDIR(st.st_mode) : shell_path_is_dir(full_path);
    char size_text[16];

    if (is_dir) {
        strlcpy(size_text, "<DIR>", sizeof(size_text));
    } else if (stat_ok) {
        shell_format_file_size(size_text,
                               sizeof(size_text),
                               st.st_size,
                               options != NULL && options->human);
    } else {
        strlcpy(size_text, "?", sizeof(size_text));
    }

    solar_os_shell_io_printf(term, "%8s ", size_text);
    if (is_dir) {
        solar_os_shell_io_write_bold(term, display_name);
        solar_os_shell_io_put_char(term, '/');
    } else {
        solar_os_shell_io_write(term, display_name);
    }
    solar_os_shell_io_put_char(term, '\n');
}

static void shell_list_directory(solar_os_shell_io_t *term,
                                 const char *path,
                                 const shell_ls_options_t *options)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        solar_os_shell_io_printf(term, "ls: cannot open %s: %s\n", path, strerror(errno));
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        char child_path[SHELL_PATH_MAX];
        if ((options == NULL || !options->show_all) && shell_ls_hidden_name(entry->d_name)) {
            continue;
        }
        if (!join_path_checked(child_path, sizeof(child_path), path, entry->d_name)) {
            solar_os_shell_io_printf(term, "%8s %s\n", "?", entry->d_name);
            continue;
        }

        shell_ls_print_entry_with_options(term, child_path, entry->d_name, options);
    }

    closedir(dir);
}

static bool shell_ls_match(solar_os_context_t *ctx,
                           const char *full_path,
                           const char *display_path,
                           const char *name,
                           void *user)
{
    solar_os_shell_io_t *term = terminal(ctx);
    const shell_ls_options_t *options = (const shell_ls_options_t *)user;

    if ((options == NULL || !options->show_all) && shell_ls_hidden_name(name)) {
        return true;
    }
    shell_ls_print_entry_with_options(term, full_path, display_path, options);
    return true;
}

static bool shell_ls_parse_options(solar_os_shell_io_t *term,
                                   int argc,
                                   char **argv,
                                   shell_ls_options_t *options,
                                   const char **path_arg)
{
    bool end_options = false;

    memset(options, 0, sizeof(*options));
    *path_arg = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!end_options && strcmp(arg, "--") == 0) {
            end_options = true;
            continue;
        }

        if (!end_options && arg[0] == '-' && arg[1] != '\0') {
            for (const char *p = &arg[1]; *p != '\0'; p++) {
                if (*p == 'a') {
                    options->show_all = true;
                } else if (*p == 'h') {
                    options->human = true;
                } else {
                    solar_os_shell_io_writeln(term, "usage: ls [-a] [-h] [path|pattern]");
                    return false;
                }
            }
            continue;
        }

        if (*path_arg != NULL) {
            solar_os_shell_io_writeln(term, "usage: ls [-a] [-h] [path|pattern]");
            return false;
        }
        *path_arg = arg;
    }

    return true;
}

void solar_os_shell_cmd_ls(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    shell_ls_options_t options;
    const char *path_arg = NULL;

    if (!shell_ls_parse_options(term, argc, argv, &options, &path_arg)) {
        return;
    }

    char path[SHELL_PATH_MAX];
    if (path_arg != NULL && shell_arg_has_wildcards(path_arg)) {
        bool had_error = false;
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, "ls", path_arg, shell_ls_match, &options, &had_error);
        shell_report_no_wildcard_matches(term, "ls", path_arg, match_count, had_error);
        return;
    }

    if (!solar_os_shell_resolve_path_for_command(ctx, term, "ls", path_arg, path, sizeof(path))) {
        return;
    }

    struct stat st;
    if (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)) {
        shell_ls_print_entry_with_options(term, path, path_arg != NULL ? path_arg : path, &options);
        return;
    }

    shell_list_directory(term, path, &options);
}

static bool shell_cat_file(solar_os_shell_io_t *term, const char *path, const char *display_path)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        solar_os_shell_io_printf(term, "cat: cannot open %s: %s\n", display_path, strerror(errno));
        return false;
    }

    char buffer[96];
    size_t bytes_read = 0;
    while (bytes_read < SHELL_CAT_MAX_BYTES && fgets(buffer, sizeof(buffer), file) != NULL) {
        bytes_read += strlen(buffer);
        solar_os_shell_io_write(term, buffer);
    }

    if (!feof(file)) {
        solar_os_shell_io_printf(term, "\ncat: %s: truncated\n", display_path);
    }

    fclose(file);
    return true;
}

typedef struct {
    solar_os_shell_io_t *term;
    bool had_error;
} shell_file_action_t;

static bool shell_cat_match(solar_os_context_t *ctx,
                            const char *full_path,
                            const char *display_path,
                            const char *name,
                            void *user)
{
    shell_file_action_t *action = (shell_file_action_t *)user;

    (void)ctx;
    (void)name;

    if (!shell_cat_file(action->term, full_path, display_path)) {
        action->had_error = true;
    }
    return true;
}

void solar_os_shell_cmd_cat(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc != 2) {
        solar_os_shell_io_writeln(term, "usage: cat <path|pattern>");
        return;
    }

    if (shell_arg_has_wildcards(argv[1])) {
        bool had_error = false;
        shell_file_action_t action = {
            .term = term,
            .had_error = false,
        };
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, "cat", argv[1], shell_cat_match, &action, &had_error);
        shell_report_no_wildcard_matches(term,
                                         "cat",
                                         argv[1],
                                         match_count,
                                         had_error || action.had_error);
        return;
    }

    char path[SHELL_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, term, "cat", argv[1], path, sizeof(path))) {
        return;
    }
    shell_cat_file(term, path, path);
}

static bool shell_make_directory(solar_os_shell_io_t *term,
                                 const char *path,
                                 const char *display_path)
{
    if (solar_os_storage_mkdir(path) != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "mkdir: cannot create %s: %s\n",
                                 display_path,
                                 strerror(errno));
        return false;
    }

    return true;
}

void solar_os_shell_cmd_mkdir(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);

    if (argc < 2) {
        solar_os_shell_io_writeln(term, "usage: mkdir <path> [path...]");
        return;
    }

    for (int i = 1; i < argc; i++) {
        if (shell_arg_has_wildcards(argv[i])) {
            solar_os_shell_io_printf(term,
                                     "mkdir: wildcards are not supported: %s\n",
                                     argv[i]);
            continue;
        }

        char path[SHELL_PATH_MAX];
        if (!solar_os_shell_resolve_path_for_command(ctx, term, "mkdir", argv[i], path, sizeof(path))) {
            continue;
        }
        shell_make_directory(term, path, path);
    }
}

typedef struct {
    bool force;
    bool recursive;
} shell_rm_options_t;

typedef struct {
    solar_os_shell_io_t *term;
    shell_rm_options_t options;
    bool had_error;
} shell_rm_action_t;

static size_t trimmed_path_len(const char *path)
{
    size_t len = path != NULL ? strlen(path) : 0;

    while (len > 1 && path[len - 1] == '/') {
        len--;
    }
    return len;
}

static bool paths_equal_trimmed(const char *a, const char *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    const size_t a_len = trimmed_path_len(a);
    const size_t b_len = trimmed_path_len(b);

    return a_len == b_len && strncmp(a, b, a_len) == 0;
}

static bool shell_path_is_protected_root(const char *path)
{
    return paths_equal_trimmed(path, "/") ||
           paths_equal_trimmed(path, solar_os_storage_mount_point());
}

static bool shell_remove_file(solar_os_shell_io_t *term,
                              const char *path,
                              const char *display_path,
                              const shell_rm_options_t *options)
{
    if (solar_os_storage_remove(path) == ESP_OK) {
        return true;
    }

    if (options != NULL && options->force && errno == ENOENT) {
        return true;
    }

    solar_os_shell_io_printf(term, "rm: cannot remove %s: %s\n", display_path, strerror(errno));
    return false;
}

static bool shell_remove_empty_directory(solar_os_shell_io_t *term,
                                         const char *path,
                                         const char *display_path)
{
    if (solar_os_storage_rmdir(path) == ESP_OK) {
        return true;
    }

    solar_os_shell_io_printf(term, "rm: cannot remove %s: %s\n", display_path, strerror(errno));
    return false;
}

static bool shell_remove_recursive(solar_os_shell_io_t *term,
                                   const char *path,
                                   const char *display_path,
                                   const shell_rm_options_t *options)
{
    if (shell_path_is_protected_root(path)) {
        solar_os_shell_io_printf(term, "rm: refusing to remove root: %s\n", display_path);
        return false;
    }

    DIR *dir = opendir(path);
    if (dir == NULL) {
        return shell_remove_file(term, path, display_path, options);
    }

    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[SHELL_PATH_MAX];
        char child_display[SHELL_PATH_MAX];
        if (!join_path_checked(child_path, sizeof(child_path), path, entry->d_name) ||
            !join_path_checked(child_display, sizeof(child_display), display_path, entry->d_name)) {
            solar_os_shell_io_printf(term, "rm: path too long below %s\n", display_path);
            ok = false;
            continue;
        }

        if (shell_path_is_dir(child_path)) {
            if (!shell_remove_recursive(term, child_path, child_display, options)) {
                ok = false;
            }
        } else if (!shell_remove_file(term, child_path, child_display, options)) {
            ok = false;
        }
    }

    closedir(dir);
    if (!shell_remove_empty_directory(term, path, display_path)) {
        ok = false;
    }
    return ok;
}

static bool shell_remove_path(solar_os_shell_io_t *term,
                              const char *path,
                              const char *display_path,
                              const shell_rm_options_t *options)
{
    if (shell_path_is_dir(path)) {
        if (options != NULL && options->recursive) {
            return shell_remove_recursive(term, path, display_path, options);
        }
        if (options != NULL && options->force) {
            return shell_remove_empty_directory(term, path, display_path);
        }

        solar_os_shell_io_printf(term,
                                 "rm: %s is a directory; use rm -f for empty dirs or rm -rf recursively\n",
                                 display_path);
        return false;
    }

    return shell_remove_file(term, path, display_path, options);
}

static bool shell_rm_match(solar_os_context_t *ctx,
                           const char *full_path,
                           const char *display_path,
                           const char *name,
                           void *user)
{
    shell_rm_action_t *action = (shell_rm_action_t *)user;

    (void)ctx;
    (void)name;

    if (!shell_remove_path(action->term, full_path, display_path, &action->options)) {
        action->had_error = true;
    }
    return true;
}

static bool shell_rm_parse_options(solar_os_shell_io_t *term,
                                   int argc,
                                   char **argv,
                                   shell_rm_options_t *options,
                                   int *first_path)
{
    memset(options, 0, sizeof(*options));
    *first_path = 1;

    while (*first_path < argc && argv[*first_path][0] == '-' && argv[*first_path][1] != '\0') {
        if (strcmp(argv[*first_path], "--") == 0) {
            (*first_path)++;
            break;
        }

        for (const char *p = &argv[*first_path][1]; *p != '\0'; p++) {
            if (*p == 'f') {
                options->force = true;
            } else if (*p == 'r' || *p == 'R') {
                options->recursive = true;
            } else {
                solar_os_shell_io_printf(term, "rm: unsupported option: -%c\n", *p);
                return false;
            }
        }
        (*first_path)++;
    }

    if (*first_path >= argc) {
        solar_os_shell_io_writeln(term, "usage: rm [-f|-rf] <path|pattern> [path|pattern...]");
        return false;
    }

    return true;
}

void solar_os_shell_cmd_rm(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    shell_rm_options_t options;
    int first_path = 1;

    if (!shell_rm_parse_options(term, argc, argv, &options, &first_path)) {
        return;
    }

    for (int i = first_path; i < argc; i++) {
        if (shell_arg_has_wildcards(argv[i])) {
            bool had_error = false;
            shell_rm_action_t action = {
                .term = term,
                .options = options,
                .had_error = false,
            };
            const size_t match_count =
                shell_for_each_wildcard_match(ctx, "rm", argv[i], shell_rm_match, &action, &had_error);
            if (!options.force) {
                shell_report_no_wildcard_matches(term,
                                                 "rm",
                                                 argv[i],
                                                 match_count,
                                                 had_error || action.had_error);
            }
            continue;
        }

        char path[SHELL_PATH_MAX];
        if (!solar_os_shell_resolve_path_for_command(ctx, term, "rm", argv[i], path, sizeof(path))) {
            continue;
        }
        shell_remove_path(term, path, path, &options);
    }
}

typedef struct {
    solar_os_shell_io_t *term;
    const char *dest_path;
    const char *command;
    bool dest_is_dir;
    bool move;
    bool had_error;
} shell_copy_move_action_t;

static bool shell_copy_or_move_path(solar_os_shell_io_t *term,
                                    const char *command,
                                    const char *source_path,
                                    const char *source_display,
                                    const char *dest_path,
                                    bool dest_is_dir,
                                    bool move,
                                    const char *name)
{
    char final_dest[SHELL_PATH_MAX];
    const char *target_path = dest_path;

    if (dest_is_dir) {
        const char *target_name = name != NULL && name[0] != '\0' ? name : shell_path_basename(source_path);
        join_path(final_dest, sizeof(final_dest), dest_path, target_name);
        target_path = final_dest;
    }

    const esp_err_t err = move ? solar_os_storage_rename(source_path, target_path) :
                                 solar_os_storage_copy_file(source_path, target_path);
    if (err != ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "%s: cannot %s %s to %s: %s\n",
                                 command,
                                 move ? "move" : "copy",
                                 source_display,
                                 target_path,
                                 strerror(errno));
        return false;
    }

    return true;
}

static bool shell_copy_move_match(solar_os_context_t *ctx,
                                  const char *full_path,
                                  const char *display_path,
                                  const char *name,
                                  void *user)
{
    shell_copy_move_action_t *action = (shell_copy_move_action_t *)user;

    (void)ctx;

    if (!shell_copy_or_move_path(action->term,
                                 action->command,
                                 full_path,
                                 display_path,
                                 action->dest_path,
                                 action->dest_is_dir,
                                 action->move,
                                 name)) {
        action->had_error = true;
    }
    return true;
}

static void shell_cmd_copy_move(solar_os_context_t *ctx, int argc, char **argv, bool move)
{
    solar_os_shell_io_t *term = terminal(ctx);
    const char *command = move ? "mv" : "cp";

    if (argc != 3) {
        solar_os_shell_io_printf(term,
                                 "usage: %s <source|pattern> <dest>\n",
                                 command);
        return;
    }
    if (shell_arg_has_wildcards(argv[2])) {
        solar_os_shell_io_printf(term,
                                 "%s: destination wildcards are not supported\n",
                                 command);
        return;
    }

    char dest[SHELL_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, term, command, argv[2], dest, sizeof(dest))) {
        return;
    }
    const bool dest_is_dir = shell_path_is_dir(dest);

    if (shell_arg_has_wildcards(argv[1])) {
        bool had_error = false;
        const size_t match_count =
            shell_for_each_wildcard_match(ctx, command, argv[1], NULL, NULL, &had_error);
        shell_report_no_wildcard_matches(term, command, argv[1], match_count, had_error);
        if (match_count == 0 || had_error) {
            return;
        }
        if (match_count > 1 && !dest_is_dir) {
            solar_os_shell_io_printf(term,
                                     "%s: destination must be a directory for multiple sources: %s\n",
                                     command,
                                     dest);
            return;
        }

        shell_copy_move_action_t action = {
            .term = term,
            .dest_path = dest,
            .command = command,
            .dest_is_dir = dest_is_dir,
            .move = move,
            .had_error = false,
        };
        (void)shell_for_each_wildcard_match(ctx,
                                            command,
                                            argv[1],
                                            shell_copy_move_match,
                                            &action,
                                            &had_error);
        return;
    }

    char source[SHELL_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, term, command, argv[1], source, sizeof(source))) {
        return;
    }
    shell_copy_or_move_path(term,
                            command,
                            source,
                            source,
                            dest,
                            dest_is_dir,
                            move,
                            shell_path_basename(source));
}

void solar_os_shell_cmd_mv(solar_os_context_t *ctx, int argc, char **argv)
{
    shell_cmd_copy_move(ctx, argc, argv, true);
}

void solar_os_shell_cmd_cp(solar_os_context_t *ctx, int argc, char **argv)
{
    shell_cmd_copy_move(ctx, argc, argv, false);
}

typedef struct {
    solar_os_shell_io_t *term;
    const char **sources;
    char (*paths)[SHELL_PATH_MAX];
    size_t count;
    size_t capacity;
    bool had_error;
} shell_zip_source_list_t;

static bool shell_zip_add_source(shell_zip_source_list_t *list,
                                 const char *command,
                                 const char *path,
                                 const char *display_path)
{
    if (list->count >= list->capacity) {
        solar_os_shell_io_printf(list->term,
                                 "%s: too many sources, limit is %u\n",
                                 command,
                                 (unsigned)list->capacity);
        list->had_error = true;
        return false;
    }

    if (strlcpy(list->paths[list->count], path, SHELL_PATH_MAX) >= SHELL_PATH_MAX) {
        solar_os_shell_io_printf(list->term,
                                 "%s: path too long: %s\n",
                                 command,
                                 display_path != NULL ? display_path : path);
        list->had_error = true;
        return false;
    }

    list->sources[list->count] = list->paths[list->count];
    list->count++;
    return true;
}

static bool shell_zip_match(solar_os_context_t *ctx,
                            const char *full_path,
                            const char *display_path,
                            const char *name,
                            void *user)
{
    shell_zip_source_list_t *list = (shell_zip_source_list_t *)user;

    (void)ctx;
    (void)name;

    return shell_zip_add_source(list, "zip", full_path, display_path);
}

typedef struct {
    solar_os_shell_io_t *term;
    bool list;
} shell_zip_progress_t;

typedef struct {
    solar_os_shell_io_t *term;
    const char *archive;
    const char **sources;
    size_t source_count;
    bool store_only;
    volatile bool done;
    esp_err_t result;
} shell_zip_create_request_t;

static const char *shell_zip_method_name(uint16_t method)
{
    switch (method) {
    case 0:
        return "store";
    case 8:
        return "deflate";
    default:
        return "?";
    }
}

static void shell_zip_progress(const solar_os_zip_event_info_t *info, void *user)
{
    shell_zip_progress_t *progress = (shell_zip_progress_t *)user;
    solar_os_shell_io_t *term = progress != NULL ? progress->term : NULL;

    if (term == NULL || info == NULL) {
        return;
    }

    if (progress->list) {
        solar_os_shell_io_printf(term,
                                 "%10" PRIu32 "  %-7s  %s\n",
                                 info->uncompressed_size,
                                 shell_zip_method_name(info->method),
                                 info->archive_name);
        return;
    }

    switch (info->event) {
    case SOLAR_OS_ZIP_EVENT_ADD:
        solar_os_shell_io_printf(term, "adding: %s\n", info->archive_name);
        break;
    case SOLAR_OS_ZIP_EVENT_DIRECTORY:
        solar_os_shell_io_printf(term, "adding: %s\n", info->archive_name);
        break;
    case SOLAR_OS_ZIP_EVENT_EXTRACT:
        solar_os_shell_io_printf(term, "extracting: %s\n", info->archive_name);
        break;
    case SOLAR_OS_ZIP_EVENT_LIST:
        break;
    }
}

static void shell_zip_create_task(void *arg)
{
    shell_zip_create_request_t *request = (shell_zip_create_request_t *)arg;
    shell_zip_progress_t progress = {
        .term = request->term,
        .list = false,
    };
    const solar_os_zip_options_t options = {
        .store_only = request->store_only,
        .progress = shell_zip_progress,
        .user = &progress,
    };

    request->result = solar_os_zip_create(request->archive,
                                          request->sources,
                                          request->source_count,
                                          &options);
    request->done = true;
    vTaskDelete(NULL);
}

static esp_err_t shell_zip_run_create_task(shell_zip_create_request_t *request)
{
    TaskHandle_t task = NULL;
    request->done = false;
    request->result = ESP_FAIL;

    const BaseType_t created = xTaskCreatePinnedToCore(shell_zip_create_task,
                                                       "solar_os_zip",
                                                       SHELL_ZIP_TASK_STACK,
                                                       request,
                                                       SHELL_ZIP_TASK_PRIORITY,
                                                       &task,
                                                       tskNO_AFFINITY);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    TickType_t poll_ticks = pdMS_TO_TICKS(SHELL_ZIP_WAIT_POLL_MS);
    if (poll_ticks == 0) {
        poll_ticks = 1;
    }
    while (!request->done) {
        vTaskDelay(poll_ticks);
    }
    return request->result;
}

static const char *shell_zip_error_reason(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_NO_MEM:
        return "no memory";
    case ESP_ERR_INVALID_ARG:
        return "invalid archive path";
    case ESP_ERR_INVALID_SIZE:
        return "ZIP64 or path size is not supported";
    case ESP_ERR_NOT_SUPPORTED:
        return "unsupported ZIP feature";
    case ESP_ERR_NOT_FOUND:
        return "not a ZIP archive";
    case ESP_ERR_INVALID_RESPONSE:
        return "corrupt ZIP archive";
    case ESP_ERR_INVALID_CRC:
        return "CRC mismatch";
    default:
        break;
    }

    return errno != 0 ? strerror(errno) : "I/O error";
}

static void shell_zip_print_error(solar_os_shell_io_t *term,
                                  const char *command,
                                  esp_err_t err)
{
    solar_os_shell_io_printf(term,
                             "%s: failed: %s\n",
                             command,
                             shell_zip_error_reason(err));
}

static bool shell_zip_alloc_sources(shell_zip_source_list_t *list, solar_os_shell_io_t *term)
{
    memset(list, 0, sizeof(*list));
    list->term = term;
    list->capacity = SHELL_ZIP_SOURCE_MAX;
    list->sources = solar_os_memory_calloc(list->capacity,
                                           sizeof(*list->sources),
                                           SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                           "shell.zip.src");
    list->paths = solar_os_memory_calloc(list->capacity,
                                         sizeof(*list->paths),
                                         SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                         "shell.zip.path");

    if (list->sources == NULL || list->paths == NULL) {
        solar_os_memory_free(list->sources);
        solar_os_memory_free(list->paths);
        memset(list, 0, sizeof(*list));
        return false;
    }
    return true;
}

static void shell_zip_free_sources(shell_zip_source_list_t *list)
{
    if (list == NULL) {
        return;
    }

    solar_os_memory_free(list->sources);
    solar_os_memory_free(list->paths);
    memset(list, 0, sizeof(*list));
}

void solar_os_shell_cmd_zip(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    bool store_only = false;
    int first_arg = 1;

    while (first_arg < argc && argv[first_arg][0] == '-' && argv[first_arg][1] != '\0') {
        if (strcmp(argv[first_arg], "--") == 0) {
            first_arg++;
            break;
        }
        if (strcmp(argv[first_arg], "-0") == 0) {
            store_only = true;
            first_arg++;
            continue;
        }

        solar_os_shell_io_printf(term, "zip: unsupported option: %s\n", argv[first_arg]);
        solar_os_shell_io_writeln(term, "usage: zip [-0] <archive.zip> <path|pattern> [path|pattern...]");
        return;
    }

    if (argc - first_arg < 2) {
        solar_os_shell_io_writeln(term, "usage: zip [-0] <archive.zip> <path|pattern> [path|pattern...]");
        return;
    }
    if (shell_arg_has_wildcards(argv[first_arg])) {
        solar_os_shell_io_writeln(term, "zip: archive path cannot contain wildcards");
        return;
    }

    char archive[SHELL_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, term, "zip", argv[first_arg], archive, sizeof(archive))) {
        return;
    }

    shell_zip_source_list_t source_list;
    if (!shell_zip_alloc_sources(&source_list, term)) {
        solar_os_shell_io_writeln(term, "zip: no memory");
        return;
    }

    for (int i = first_arg + 1; i < argc && !source_list.had_error; i++) {
        if (shell_arg_has_wildcards(argv[i])) {
            bool had_error = false;
            const size_t match_count =
                shell_for_each_wildcard_match(ctx, "zip", argv[i], shell_zip_match, &source_list, &had_error);
            shell_report_no_wildcard_matches(term,
                                             "zip",
                                             argv[i],
                                             match_count,
                                             had_error || source_list.had_error);
            continue;
        }

        char source[SHELL_PATH_MAX];
        if (!solar_os_shell_resolve_path_for_command(ctx, term, "zip", argv[i], source, sizeof(source))) {
            source_list.had_error = true;
            break;
        }
        (void)shell_zip_add_source(&source_list, "zip", source, argv[i]);
    }

    if (!source_list.had_error && source_list.count == 0) {
        solar_os_shell_io_writeln(term, "zip: no input files");
        source_list.had_error = true;
    }

    if (!source_list.had_error) {
        shell_zip_create_request_t request = {
            .term = term,
            .archive = archive,
            .sources = source_list.sources,
            .source_count = source_list.count,
            .store_only = store_only,
        };
        const esp_err_t err = shell_zip_run_create_task(&request);
        if (err != ESP_OK) {
            shell_zip_print_error(term, "zip", err);
        }
    }

    shell_zip_free_sources(&source_list);
}

void solar_os_shell_cmd_unzip(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    bool list_only = false;
    int first_arg = 1;

    while (first_arg < argc && argv[first_arg][0] == '-' && argv[first_arg][1] != '\0') {
        if (strcmp(argv[first_arg], "--") == 0) {
            first_arg++;
            break;
        }
        if (strcmp(argv[first_arg], "-l") == 0) {
            list_only = true;
            first_arg++;
            continue;
        }

        solar_os_shell_io_printf(term, "unzip: unsupported option: %s\n", argv[first_arg]);
        solar_os_shell_io_writeln(term, "usage: unzip [-l] <archive.zip> [dest]");
        return;
    }

    if ((list_only && argc - first_arg != 1) || (!list_only && (argc - first_arg < 1 || argc - first_arg > 2))) {
        solar_os_shell_io_writeln(term, "usage: unzip [-l] <archive.zip> [dest]");
        return;
    }

    char archive[SHELL_PATH_MAX];
    if (!solar_os_shell_resolve_path_for_command(ctx, term, "unzip", argv[first_arg], archive, sizeof(archive))) {
        return;
    }

    shell_zip_progress_t progress = {
        .term = term,
        .list = list_only,
    };

    esp_err_t err = ESP_OK;
    if (list_only) {
        solar_os_shell_io_writeln(term, "    Length  Method   Name");
        err = solar_os_zip_list(archive, shell_zip_progress, &progress);
    } else {
        char dest[SHELL_PATH_MAX];
        const char *dest_arg = argc - first_arg == 2 ? argv[first_arg + 1] : NULL;
        if (!solar_os_shell_resolve_path_for_command(ctx, term, "unzip", dest_arg, dest, sizeof(dest))) {
            return;
        }
        const solar_os_unzip_options_t options = {
            .progress = shell_zip_progress,
            .user = &progress,
        };
        err = solar_os_zip_extract(archive, dest, &options);
    }

    if (err != ESP_OK) {
        shell_zip_print_error(term, "unzip", err);
    }
}
