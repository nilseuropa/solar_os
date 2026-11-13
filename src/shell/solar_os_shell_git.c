#include "solar_os_shell_commands.h"

#include <stdatomic.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_git.h"
#include "solar_os_input.h"
#include "solar_os_keys.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define SOLAR_OS_GIT_STACK 28672U
SOLAR_OS_TASK_REQUIRE_FOREGROUND_STACK(SOLAR_OS_GIT_STACK);

typedef struct {
    solar_os_shell_io_t *io;
    char cwd[SOLAR_OS_STORAGE_PATH_MAX];
    int argc;
    char **argv;
    atomic_bool done;
    atomic_bool stop;
    int result;
    char error[256];
} solar_os_shell_git_request_t;

static void git_output(void *user, const char *data, size_t size)
{
    solar_os_shell_git_request_t *r = user;
    solar_os_shell_io_write_len(r->io, data, size);
}

static bool git_cancelled(void *user)
{
    return atomic_load(&((solar_os_shell_git_request_t *)user)->stop);
}

static void git_worker(void *user)
{
    solar_os_shell_git_request_t *r = user;
    const solar_os_git_options_t opts = { .user = r, .output = git_output,
        .cancelled = git_cancelled, .error = r->error, .error_size = sizeof(r->error) };
    r->result = solar_os_git_run(r->cwd, r->argc, r->argv, &opts);
    solar_os_storage_notify_changed();
    atomic_store(&r->done, true);
    solar_os_task_delete_internal(NULL);
}

static bool git_exit_key(solar_os_shell_io_t *io)
{
    uint8_t data[16];
    size_t n = 0;
    if (solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_PORT) {
        if (solar_os_port_read(&io->port, data, sizeof(data), 0, &n) != ESP_OK) return false;
    } else n = solar_os_input_read_chars((char *)data, sizeof(data));
    for (size_t i = 0; i < n; i++)
        if (data[i] == 0x1d || data[i] == SOLAR_OS_KEY_APP_EXIT) return true;
    return false;
}

void solar_os_shell_cmd_git(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_context_io(ctx);
    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "--help")) {
        solar_os_shell_io_writeln(io,
            "git init [directory]\n"
            "git clone [-b branch] <ssh-or-https-url> [directory]\n"
            "git config <user.name|user.email> <value>\n"
            "git status | add <path>... | diff [--cached]\n"
            "git commit -m <message> | log [-n count]\n"
            "git branch [name | -d name] | checkout [-b] <branch>\n"
            "git switch [-c] <branch>\n"
            "git remote [-v | add <name> <url> | set-url <name> <url>]\n"
            "git fetch [remote] | pull [remote [branch]] | push [-u] [remote [branch]]\n"
            "pull is fast-forward only. SSH uses sshkey; HTTPS is anonymous.\n"
            "Ctrl+] cancels. See man git.");
        return;
    }
    solar_os_shell_git_request_t request = { .io = io, .argc = argc - 1, .argv = argv + 1 };
    atomic_init(&request.done, false);
    atomic_init(&request.stop, false);
    if (solar_os_shell_resolve_path(ctx, ".", request.cwd, sizeof(request.cwd)) != ESP_OK) {
        solar_os_shell_io_writeln(io, "git: cannot resolve working directory");
        solar_os_shell_session_set_exit_result(ctx->shell_session, 1, NULL);
        return;
    }
    if (solar_os_task_create_pinned_internal(git_worker, "solar_os_git", SOLAR_OS_GIT_STACK,
            &request, 4, NULL, tskNO_AFFINITY, SOLAR_OS_TASK_ROLE_FOREGROUND) != pdPASS) {
        solar_os_shell_io_writeln(io, "git: no memory for worker");
        solar_os_shell_session_set_exit_result(ctx->shell_session, 1, NULL);
        return;
    }
    while (!atomic_load(&request.done)) {
        if (git_exit_key(io)) atomic_store(&request.stop, true);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (request.result) {
        solar_os_shell_io_printf(io, "git: %s\n", request.error[0] ? request.error : "operation failed");
        solar_os_shell_session_set_exit_result(ctx->shell_session, 1, NULL);
    }
}
