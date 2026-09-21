#include "solar_os_sftpsync_app.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_ble_keyboard.h"
#include "solar_os_identity.h"
#include "solar_os_sftpsync.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_ssh_keys.h"
#include "solar_os_terminal.h"

#define SFTPSYNC_DEFAULT_PORT 22
#define SFTPSYNC_PROGRESS_BAR_MAX 24U

typedef enum {
    SFTPSYNC_APP_PASSWORD,
    SFTPSYNC_APP_RUNNING,
} sftpsync_app_mode_t;

typedef struct {
    bool remote;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char host[SOLAR_OS_SSH_HOST_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
} sftpsync_target_t;

typedef struct {
    solar_os_sftpsync_session_t *session;
    solar_os_sftpsync_config_t config;
    solar_os_sftpsync_direction_t direction;
    sftpsync_app_mode_t mode;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    char password[SOLAR_OS_SSH_PASSWORD_MAX];
    size_t password_len;
    uint16_t port;
    bool recursive;
    bool dry_run;
    bool failed;
    bool progress_active;
    size_t progress_row;
    uint8_t progress_percent;
    char progress_path[SOLAR_OS_SFTPSYNC_EVENT_MESSAGE_MAX];
} sftpsync_app_state_t;

typedef struct {
    sftpsync_app_state_t app;
    solar_os_shell_io_t fallback_io;
} sftpsync_app_cold_state_t;

static void *sftpsync_app_state;
#define sftpsync_app (((sftpsync_app_cold_state_t *)sftpsync_app_state)->app)
#define sftpsync_fallback_io (((sftpsync_app_cold_state_t *)sftpsync_app_state)->fallback_io)

static solar_os_shell_io_t *sftpsync_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&sftpsync_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &sftpsync_fallback_io);
        io = &sftpsync_fallback_io;
    }
    return io;
}

static void sftpsync_finish(solar_os_context_t *ctx, int exit_code, const char *message)
{
    solar_os_context_finish(ctx, exit_code, message);
}

static bool sftpsync_parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || port == NULL) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static bool sftpsync_parse_remote(const char *arg, sftpsync_target_t *target)
{
    const char *colon = arg != NULL ? strchr(arg, ':') : NULL;
    if (colon == NULL || colon == arg || colon[1] == '\0') {
        return false;
    }
    const size_t authority_len = (size_t)(colon - arg);
    if (authority_len >= SOLAR_OS_SSH_USERNAME_MAX + SOLAR_OS_SSH_HOST_MAX + 2 ||
        strlen(colon + 1) >= sizeof(target->path)) {
        return false;
    }
    char authority[SOLAR_OS_SSH_USERNAME_MAX + SOLAR_OS_SSH_HOST_MAX + 2];
    memcpy(authority, arg, authority_len);
    authority[authority_len] = '\0';

    char *host = authority;
    char *at = strchr(authority, '@');
    if (at != NULL) {
        if (at == authority || at[1] == '\0') {
            return false;
        }
        *at = '\0';
        host = at + 1;
        strlcpy(target->username, authority, sizeof(target->username));
    } else {
        solar_os_identity_get_user(target->username, sizeof(target->username));
    }
    target->remote = true;
    strlcpy(target->host, host, sizeof(target->host));
    strlcpy(target->path, colon + 1, sizeof(target->path));
    return target->username[0] != '\0' && target->host[0] != '\0';
}

static bool sftpsync_parse_target(solar_os_context_t *ctx,
                               const char *arg,
                               sftpsync_target_t *target)
{
    memset(target, 0, sizeof(*target));
    if (sftpsync_parse_remote(arg, target)) {
        return true;
    }
    if (arg == NULL || arg[0] == '\0') {
        return false;
    }
    return solar_os_shell_resolve_path(ctx, arg, target->path, sizeof(target->path)) == ESP_OK &&
        target->path[0] != '\0';
}

static void sftpsync_usage(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = sftpsync_io(ctx);
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  sftpsync [-r|-a] [-n] [-P port] local [user@]host:remote");
    solar_os_shell_io_writeln(io, "  sftpsync [-r|-a] [-n] [-P port] [user@]host:remote local");
    solar_os_shell_io_writeln(io, "options:");
    solar_os_shell_io_writeln(io, "  -r, -a, --recursive  recurse into directories");
    solar_os_shell_io_writeln(io, "  -n, --dry-run        show changes without writing");
    solar_os_shell_io_writeln(io, "  -P port              SSH port (default 22)");
    solar_os_shell_io_writeln(io, "This client uses SFTP; unchanged size/mtime pairs are skipped.");
    solar_os_shell_io_flush(io);
    sftpsync_finish(ctx, 2, NULL);
}

static bool sftpsync_parse_short_options(const char *arg,
                                         bool *recursive,
                                         bool *dry_run)
{
    if (arg == NULL || recursive == NULL || dry_run == NULL ||
        arg[0] != '-' || arg[1] == '\0') {
        return false;
    }

    bool parsed_recursive = false;
    bool parsed_dry_run = false;
    for (const char *option = arg + 1; *option != '\0'; option++) {
        if (*option == 'r' || *option == 'a') {
            parsed_recursive = true;
        } else if (*option == 'n') {
            parsed_dry_run = true;
        } else {
            return false;
        }
    }

    *recursive = *recursive || parsed_recursive;
    *dry_run = *dry_run || parsed_dry_run;
    return true;
}

static bool sftpsync_parse_args(solar_os_context_t *ctx)
{
    const int argc = solar_os_context_argc(ctx);
    const char *operands[2] = {0};
    int operand_count = 0;
    sftpsync_app.port = SFTPSYNC_DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        const char *arg = solar_os_context_argv(ctx, i);
        if (strcmp(arg, "-P") == 0) {
            if (++i >= argc || !sftpsync_parse_port(solar_os_context_argv(ctx, i), &sftpsync_app.port)) {
                return false;
            }
        } else if (strcmp(arg, "--recursive") == 0) {
            sftpsync_app.recursive = true;
        } else if (strcmp(arg, "--dry-run") == 0) {
            sftpsync_app.dry_run = true;
        } else if (arg[0] == '-') {
            if (!sftpsync_parse_short_options(arg,
                                              &sftpsync_app.recursive,
                                              &sftpsync_app.dry_run)) {
                return false;
            }
        } else if (operand_count < 2) {
            operands[operand_count++] = arg;
        } else {
            return false;
        }
    }
    if (operand_count != 2) {
        return false;
    }

    sftpsync_target_t source;
    sftpsync_target_t destination;
    if (!sftpsync_parse_target(ctx, operands[0], &source) ||
        !sftpsync_parse_target(ctx, operands[1], &destination) ||
        source.remote == destination.remote) {
        return false;
    }
    const sftpsync_target_t *remote = source.remote ? &source : &destination;
    const sftpsync_target_t *local = source.remote ? &destination : &source;
    sftpsync_app.direction = source.remote ? SOLAR_OS_SFTPSYNC_DOWNLOAD : SOLAR_OS_SFTPSYNC_UPLOAD;
    strlcpy(sftpsync_app.host, remote->host, sizeof(sftpsync_app.host));
    strlcpy(sftpsync_app.username, remote->username, sizeof(sftpsync_app.username));
    strlcpy(sftpsync_app.remote_path, remote->path, sizeof(sftpsync_app.remote_path));
    strlcpy(sftpsync_app.local_path, local->path, sizeof(sftpsync_app.local_path));
    return true;
}

static void sftpsync_password_prompt(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = sftpsync_io(ctx);
    solar_os_shell_io_printf_bold(io,
                                  "sftpsync %s %s@%s:%u%s\n",
                                  sftpsync_app.direction == SOLAR_OS_SFTPSYNC_UPLOAD ? "push" : "pull",
                                  sftpsync_app.username,
                                  sftpsync_app.host,
                                  (unsigned)sftpsync_app.port,
                                  sftpsync_app.dry_run ? " (dry run)" : "");
    solar_os_shell_io_write(io,
                            solar_os_ssh_keys_default_exists() ?
                                "password (Enter for key): " : "password: ");
    solar_os_shell_io_flush(io);
}

static esp_err_t sftpsync_begin(solar_os_context_t *ctx)
{
    sftpsync_app.config = (solar_os_sftpsync_config_t){
        .direction = sftpsync_app.direction,
        .host = sftpsync_app.host,
        .port = sftpsync_app.port,
        .username = sftpsync_app.username,
        .password = sftpsync_app.password,
        .local_path = sftpsync_app.local_path,
        .remote_path = sftpsync_app.remote_path,
        .recursive = sftpsync_app.recursive,
        .dry_run = sftpsync_app.dry_run,
    };
    solar_os_shell_io_t *io = sftpsync_io(ctx);
    solar_os_shell_io_printf(io, "local:  %s\n", sftpsync_app.local_path);
    solar_os_shell_io_printf(io, "remote: %s\n", sftpsync_app.remote_path);
    solar_os_shell_io_flush(io);

    const esp_err_t err = solar_os_sftpsync_start(&sftpsync_app.config, &sftpsync_app.session);
    memset(sftpsync_app.password, 0, sizeof(sftpsync_app.password));
    sftpsync_app.password_len = 0;
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "sftpsync start failed: %s\n", esp_err_to_name(err));
        solar_os_shell_io_flush(io);
        sftpsync_finish(ctx, 1, "sftpsync: failed");
        return err;
    }
    sftpsync_app.mode = SFTPSYNC_APP_RUNNING;
    return ESP_OK;
}

static void sftpsync_progress_finish_line(solar_os_shell_io_t *io)
{
    if (!sftpsync_app.progress_active) {
        return;
    }
    if (solar_os_shell_io_is_cursor_addressable(io)) {
        solar_os_shell_io_newline(io);
    }
    sftpsync_app.progress_active = false;
    sftpsync_app.progress_path[0] = '\0';
}

static void sftpsync_progress_render(solar_os_shell_io_t *io,
                                     const solar_os_sftpsync_event_t *event)
{
    if (event->message[0] == '\0') {
        return;
    }
    if (sftpsync_app.progress_active &&
        strcmp(sftpsync_app.progress_path, event->message) != 0) {
        sftpsync_progress_finish_line(io);
    }

    uint8_t percent = 0U;
    if (event->file_size_known) {
        percent = event->file_size == 0U || event->file_transferred >= event->file_size ?
            100U : (uint8_t)((event->file_transferred * 100U) / event->file_size);
    }
    if (!sftpsync_app.progress_active) {
        sftpsync_app.progress_active = true;
        sftpsync_app.progress_row = solar_os_shell_io_cursor_row(io);
        sftpsync_app.progress_percent = UINT8_MAX;
        strlcpy(sftpsync_app.progress_path,
                event->message,
                sizeof(sftpsync_app.progress_path));
    }

    const size_t reported_cols = solar_os_shell_io_cols(io);
    const size_t cols = reported_cols >= 20U ? reported_cols : 20U;
    const size_t line_width = cols > 1U ? cols - 1U : cols;
    size_t bar_width = line_width > 20U ? line_width - 20U : 8U;
    if (bar_width > SFTPSYNC_PROGRESS_BAR_MAX) {
        bar_width = SFTPSYNC_PROGRESS_BAR_MAX;
    }
    const size_t fixed_width = bar_width + 8U;
    const size_t label_width = line_width > fixed_width ? line_width - fixed_width : 1U;
    const size_t path_len = strlen(event->message);
    const char *label = path_len > label_width ?
        event->message + path_len - label_width : event->message;
    const size_t filled = event->file_size_known ?
        ((size_t)percent * bar_width) / 100U : 0U;

    char line[256];
    size_t used = 0U;
    line[used++] = '[';
    for (size_t i = 0U; i < bar_width && used + 1U < sizeof(line); i++) {
        line[used++] = event->file_size_known ? (i < filled ? '#' : '-') : '?';
    }
    int written = 0;
    if (event->file_size_known) {
        written = snprintf(line + used,
                           sizeof(line) - used,
                           "] %3u%% %.*s",
                           (unsigned)percent,
                           (int)label_width,
                           label);
    } else {
        written = snprintf(line + used,
                           sizeof(line) - used,
                           "]  --%% %.*s",
                           (int)label_width,
                           label);
    }
    if (written <= 0 || (size_t)written >= sizeof(line) - used) {
        return;
    }
    used += (size_t)written;

    if (solar_os_shell_io_is_cursor_addressable(io)) {
        (void)solar_os_shell_io_redraw_line(io,
                                            sftpsync_app.progress_row,
                                            0U,
                                            line,
                                            used,
                                            used);
    } else if (sftpsync_app.progress_percent == UINT8_MAX ||
               percent == 100U ||
               percent >= (uint8_t)(sftpsync_app.progress_percent + 10U)) {
        solar_os_shell_io_writeln(io, line);
    }
    sftpsync_app.progress_percent = percent;
}

static void sftpsync_drain_events(solar_os_context_t *ctx)
{
    if (sftpsync_app.session == NULL) {
        return;
    }
    solar_os_shell_io_t *io = sftpsync_io(ctx);
    solar_os_sftpsync_event_t event;
    while (sftpsync_app.session != NULL && solar_os_sftpsync_poll(sftpsync_app.session, &event)) {
        switch (event.type) {
        case SOLAR_OS_SFTPSYNC_EVENT_STATUS:
            sftpsync_progress_finish_line(io);
            solar_os_shell_io_printf(io, "sftpsync: %s\n", event.message);
            break;
        case SOLAR_OS_SFTPSYNC_EVENT_PROGRESS:
            sftpsync_progress_render(io, &event);
            break;
        case SOLAR_OS_SFTPSYNC_EVENT_ERROR:
            sftpsync_progress_finish_line(io);
            sftpsync_app.failed = true;
            solar_os_shell_io_printf(io, "sftpsync: %s\n", event.message);
            break;
        case SOLAR_OS_SFTPSYNC_EVENT_DONE:
            sftpsync_progress_finish_line(io);
            solar_os_shell_io_printf(io,
                                     "sftpsync: %s (%u changed, %" PRIu64 " bytes)\n",
                                     event.message,
                                     (unsigned)event.files_changed,
                                     event.transferred);
            solar_os_sftpsync_stop(sftpsync_app.session);
            sftpsync_app.session = NULL;
            sftpsync_finish(ctx,
                         sftpsync_app.failed ? 1 : 0,
                         sftpsync_app.failed ? "sftpsync: failed" : NULL);
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t sftpsync_start(solar_os_context_t *ctx)
{
    memset(&sftpsync_app, 0, sizeof(sftpsync_app));
    if (!sftpsync_parse_args(ctx)) {
        sftpsync_usage(ctx);
        return ESP_OK;
    }
    sftpsync_app.mode = SFTPSYNC_APP_PASSWORD;
    sftpsync_password_prompt(ctx);
    return ESP_OK;
}

static void sftpsync_stop(solar_os_context_t *ctx)
{
    if (ctx != NULL) {
        sftpsync_progress_finish_line(sftpsync_io(ctx));
    }
    if (sftpsync_app.session != NULL) {
        solar_os_sftpsync_stop(sftpsync_app.session);
    }
    memset(&sftpsync_app, 0, sizeof(sftpsync_app));
}

static bool sftpsync_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }
    if (event->type == SOLAR_OS_EVENT_TICK) {
        sftpsync_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT || (uint8_t)ch == 0x03U) {
        sftpsync_progress_finish_line(sftpsync_io(ctx));
        const bool running = sftpsync_app.session != NULL;
        if (running) {
            solar_os_sftpsync_stop(sftpsync_app.session);
            sftpsync_app.session = NULL;
        }
        sftpsync_finish(ctx, running ? 130 : 0, running ? "sftpsync: cancelled" : NULL);
        return true;
    }
    if (sftpsync_app.mode == SFTPSYNC_APP_PASSWORD) {
        if (ch == '\b') {
            if (sftpsync_app.password_len > 0) {
                sftpsync_app.password[--sftpsync_app.password_len] = '\0';
                solar_os_shell_io_write(sftpsync_io(ctx), "\b \b");
                solar_os_shell_io_flush(sftpsync_io(ctx));
            }
        } else if (ch == '\r' || ch == '\n') {
            solar_os_shell_io_newline(sftpsync_io(ctx));
            (void)sftpsync_begin(ctx);
        } else if ((isprint((unsigned char)ch) || (unsigned char)ch >= 0xa0) &&
                   sftpsync_app.password_len + 1 < sizeof(sftpsync_app.password)) {
            sftpsync_app.password[sftpsync_app.password_len++] = ch;
            sftpsync_app.password[sftpsync_app.password_len] = '\0';
            solar_os_shell_io_put_char(sftpsync_io(ctx), '*');
            solar_os_shell_io_flush(sftpsync_io(ctx));
        }
    } else {
        sftpsync_drain_events(ctx);
    }
    return true;
}

const solar_os_app_t solar_os_sftpsync_app = {
    .name = "sftpsync",
    .summary = "synchronize files over SSH",
    .app_class = SOLAR_OS_APP_CLASS_COMMAND,
    .flags = SOLAR_OS_APP_FLAG_SHELL_INLINE,
    .start = sftpsync_start,
    .stop = sftpsync_stop,
    .event = sftpsync_event,
    .state_slot = &sftpsync_app_state,
    .state_size = sizeof(sftpsync_app_cold_state_t),
    .state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED,
    .worker_stack_bytes = SOLAR_OS_SFTPSYNC_TASK_STACK,
};
