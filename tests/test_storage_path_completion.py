from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]


class StoragePathCompletionTest(unittest.TestCase):
    def test_mounted_usb_root_files_directories_and_quoted_names(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        work_start = source.index("typedef struct {\n    char original[SHELL_INPUT_MAX];")
        work_end = source.index("} shell_path_completion_work_t;", work_start)
        work_type = source[work_start:work_end + len("} shell_path_completion_work_t;")]
        helpers = production_functions(source, (
            "starts_with", "shell_arg_has_wildcards", "shell_wildcard_match",
            "join_path", "shell_path_is_dir", "shell_path_entry_is_dir",
            "shell_print_path_match", "shell_alloc_path_completion_work",
            "shell_update_common_prefix", "shell_path_entry_matches",
            "shell_path_completion_mount_entry", "shell_path_completion_is_mount",
            "shell_path_completion_add_match", "shell_print_path_matches",
            "shell_complete_path",
        ))
        code = r'''
#include <assert.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_shell_completion.h"

#define SHELL_PATH_MAX 512
#define SHELL_INPUT_MAX 1024
#define SOLAR_OS_MEMORY_TRANSIENT 0
#define ESP_OK 0
typedef struct {
    char input[SHELL_INPUT_MAX];
    size_t input_len;
    bool history_browsing;
    int history_index;
} solar_os_context_t;
typedef int solar_os_shell_io_t;
typedef struct { char mount_point[64]; } solar_os_storage_mount_info_t;
static char fixture[SHELL_PATH_MAX], output[4096];
static bool usb_mounted = true, default_mounted = true, usb_cwd;
static solar_os_shell_io_t io;
static solar_os_context_t *shell_session(solar_os_context_t *ctx) { return ctx; }
static solar_os_shell_io_t *shell_io(solar_os_context_t *ctx) { (void)ctx; return &io; }
static void shell_prompt(solar_os_context_t *ctx) { (void)ctx; }
static void shell_replace_input(solar_os_context_t *ctx, const char *text) {
    strcpy(ctx->input, text); ctx->input_len = strlen(text);
}
static void solar_os_shell_io_write(solar_os_shell_io_t *unused, const char *text) {
    (void)unused; assert(strlen(output) + strlen(text) < sizeof(output)); strcat(output, text);
}
static void solar_os_shell_io_put_char(solar_os_shell_io_t *unused, char ch) {
    char text[] = {ch, 0}; solar_os_shell_io_write(unused, text);
}
static void solar_os_shell_io_newline(solar_os_shell_io_t *unused) {
    solar_os_shell_io_put_char(unused, '\n');
}
static void *solar_os_memory_calloc(size_t n, size_t size, int cls, const char *tag) {
    (void)cls; (void)tag; return calloc(n, size);
}
static void solar_os_memory_free(void *p) { free(p); }
static size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src);
    if (size) { size_t n = len < size - 1 ? len : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return len;
}
static size_t solar_os_storage_mount_count(void) { return usb_mounted ? 2 : 1; }
static bool solar_os_storage_get_mount(size_t index, solar_os_storage_mount_info_t *mount) {
    if (index >= solar_os_storage_mount_count()) return false;
    strcpy(mount->mount_point, index == 0 ? "/flash" : "/usb0"); return true;
}
static int resolve_path(solar_os_context_t *ctx, const char *arg, char *path, size_t len) {
    (void)ctx;
    if (arg && !strncmp(arg, "/usb0", 5) && (arg[5] == '/' || arg[5] == 0)) {
        if (!usb_mounted) return -1;
        snprintf(path, len, "%s/usb%s", fixture, arg + 5);
    } else if (!arg && usb_cwd) {
        if (!usb_mounted) return -1;
        snprintf(path, len, "%s/usb", fixture);
    } else {
        if (!default_mounted) return -1;
        snprintf(path, len, "%s/default", fixture);
    }
    return ESP_OK;
}
''' + work_type + "\n" + helpers + r'''
static void check(const char *line, const char *token, bool dirs_only,
                  bool show_matches, const char *expected) {
    solar_os_context_t ctx = {0};
    shell_replace_input(&ctx, line); output[0] = 0;
    shell_complete_path(&ctx, (size_t)(strchr(line, ' ') - line + 1), token,
                        dirs_only, show_matches);
    if (strcmp(ctx.input, expected)) {
        fprintf(stderr, "%s -> %s; expected %s\n", line, ctx.input, expected); abort();
    }
}
int main(int argc, char **argv) {
    assert(argc == 2); strlcpy(fixture, argv[1], sizeof(fixture));
    check("ls /us", "/us", false, false, "ls /usb0/");
    check("cd /usb0", "/usb0", true, false, "cd /usb0/");
    check("cat /usb0/rep", "/usb0/rep", false, false, "cat /usb0/report.txt ");
    check("cd /usb0/Do", "/usb0/Do", true, false, "cd /usb0/Documents/");
    check("cat /usb0/Documents/ne", "/usb0/Documents/ne", false, false,
          "cat /usb0/Documents/nested.txt ");
    check("cat /usb0/My", "/usb0/My", false, false, "cat \"/usb0/My File.txt\" ");
    check("cat \"/usb0/My F", "/usb0/My F", false, false, "cat \"/usb0/My File.txt\" ");
    check("cat /usb0/r", "/usb0/r", false, false, "cat /usb0/re");
    check("cat /usb0/re", "/usb0/re", false, true, "cat /usb0/re");
    assert(strstr(output, "report.txt\n") && strstr(output, "readme.txt\n"));
    check("cat /usb0/*.txt", "/usb0/*.txt", false, true, "cat /usb0/*.txt");
    assert(strstr(output, "My File.txt\n") && strstr(output, "report.txt\n"));
    check("cd /usb0/rep", "/usb0/rep", true, false, "cd /usb0/rep");
    check("ls /", "/", false, true, "ls /");
    const char *usb = strstr(output, "usb0/\n");
    assert(usb && !strstr(usb + 1, "usb0/\n"));
    assert(strstr(output, "flash/\n") && strstr(output, "local.txt\n"));
    check("cat /lo", "/lo", false, false, "cat /local.txt ");
    usb_cwd = true;
    check("cat rep", "rep", false, false, "cat report.txt ");
    usb_cwd = false; default_mounted = false;
    check("cd /us", "/us", true, false, "cd /usb0/");
    check("ls /", "/", true, true, "ls /");
    assert(strstr(output, "usb0/\n"));
    check("cat /usb0/rep", "/usb0/rep", false, false, "cat /usb0/report.txt ");
    usb_mounted = false;
    check("cd /us", "/us", true, false, "cd /us");
    check("cat /usb0/rep", "/usb0/rep", false, false, "cat /usb0/rep");
    puts("storage path completion tests: ok");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "default").mkdir()
            (stage / "default/local.txt").write_text("local")
            # A physical entry with the same name must not duplicate or hide the mount.
            (stage / "default/usb0").write_text("shadowed file")
            (stage / "usb/Documents").mkdir(parents=True)
            for name in ("report.txt", "readme.txt", "My File.txt", "Documents/nested.txt"):
                (stage / "usb" / name).write_text("USB file")
            binary = stage / "completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-I", str(ROOT / "src/shell"), "-o", str(binary), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"),
            ], input=code, text=True, check=True)
            subprocess.run([str(binary), str(stage)], check=True)
