#include "solar_os_scp_app.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "solar_os_ble_keyboard.h"
#include "solar_os_identity.h"
#include "solar_os_scp.h"
#include "solar_os_shell.h"
#include "solar_os_shell_io.h"
#include "solar_os_ssh.h"
#include "solar_os_ssh_keys.h"
#include "solar_os_storage.h"
#include "solar_os_terminal.h"

#define SCP_PASSWORD_PROMPT_MAX SOLAR_OS_SSH_PASSWORD_MAX
#define SCP_DEFAULT_PORT 22

typedef enum {
    SCP_APP_PASSWORD,
    SCP_APP_RUNNING,
    SCP_APP_ERROR,
} scp_app_mode_t;

typedef struct {
    bool remote;
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char host[SOLAR_OS_SSH_HOST_MAX];
    char path[SOLAR_OS_STORAGE_PATH_MAX];
} scp_target_t;

typedef struct {
    solar_os_scp_session_t *session;
    solar_os_scp_config_t config;
    solar_os_scp_direction_t direction;
    scp_app_mode_t mode;
    char host[SOLAR_OS_SSH_HOST_MAX];
    char username[SOLAR_OS_SSH_USERNAME_MAX];
    char local_path[SOLAR_OS_STORAGE_PATH_MAX];
    char remote_path[SOLAR_OS_STORAGE_PATH_MAX];
    char password[SCP_PASSWORD_PROMPT_MAX];
    size_t password_len;
    uint16_t port;
    int last_percent;
    uint64_t last_progress_step;
    bool saw_error;
} scp_app_state_t;

static scp_app_state_t scp_app;
static solar_os_shell_io_t scp_fallback_io;

static solar_os_shell_io_t *scp_io(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = solar_os_context_shell_io(ctx);
    if (io == NULL || solar_os_shell_io_kind(io) == SOLAR_OS_SHELL_IO_KIND_NONE) {
        solar_os_shell_io_init_terminal(&scp_fallback_io, solar_os_context_terminal(ctx));
        solar_os_context_set_shell_io(ctx, &scp_fallback_io);
        io = &scp_fallback_io;
    }
    return io;
}

static bool scp_is_printable(char ch)
{
    const unsigned char value = (unsigned char)ch;
    return isprint(value) || value >= 0xa0;
}

static bool scp_parse_port(const char *text, uint16_t *port)
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

static bool scp_parse_remote(const char *arg, scp_target_t *target)
{
    if (arg == NULL || target == NULL) {
        return false;
    }

    const char *colon = strchr(arg, ':');
    if (colon == NULL || colon == arg) {
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

    if (host[0] == '\0') {
        return false;
    }

    target->remote = true;
    strlcpy(target->host, host, sizeof(target->host));
    strlcpy(target->path, colon + 1, sizeof(target->path));
    return target->username[0] != '\0' && target->host[0] != '\0';
}

static bool scp_parse_target(solar_os_context_t *ctx, const char *arg, scp_target_t *target)
{
    memset(target, 0, sizeof(*target));
    if (scp_parse_remote(arg, target)) {
        return true;
    }

    if (arg == NULL || arg[0] == '\0') {
        return false;
    }

    target->remote = false;
    return solar_os_shell_resolve_path(ctx, arg, target->path, sizeof(target->path)) == ESP_OK &&
        target->path[0] != '\0';
}

static const char *scp_remote_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    if (slash != NULL && slash[1] != '\0') {
        return slash + 1;
    }
    return path != NULL && path[0] != '\0' ? path : "scp.out";
}

static const char *scp_local_basename(const char *path)
{
    const char *slash = path != NULL ? strrchr(path, '/') : NULL;
    if (slash != NULL && slash[1] != '\0') {
        return slash + 1;
    }
    return path != NULL && path[0] != '\0' ? path : "scp.out";
}

static bool scp_append_path(char *path, size_t path_len, const char *dir, const char *name)
{
    if (path == NULL || path_len == 0 || dir == NULL || name == NULL || name[0] == '\0') {
        return false;
    }

    const size_t dir_len = strlen(dir);
    const int written = snprintf(path,
                                 path_len,
                                 "%s%s%s",
                                 dir,
                                 dir_len > 0 && dir[dir_len - 1] == '/' ? "" : "/",
                                 name);
    return written >= 0 && (size_t)written < path_len;
}

static bool scp_prepare_local_download_path(solar_os_context_t *ctx,
                                            const char *arg,
                                            const char *remote_path,
                                            char *local_path,
                                            size_t local_path_len)
{
    const char *target = arg != NULL && arg[0] != '\0' ? arg : ".";
    if (solar_os_shell_resolve_path(ctx, target, local_path, local_path_len) != ESP_OK) {
        return false;
    }
    if (local_path[0] == '\0') {
        return false;
    }

    struct stat st;
    if (stat(local_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        const char *name = scp_remote_basename(remote_path);
        char dir[SOLAR_OS_STORAGE_PATH_MAX];
        strlcpy(dir, local_path, sizeof(dir));
        return scp_append_path(local_path, local_path_len, dir, name);
    }
    return true;
}

static bool scp_prepare_remote_upload_path(const char *remote_arg,
                                           const char *local_path,
                                           char *remote_path,
                                           size_t remote_path_len)
{
    const char *name = scp_local_basename(local_path);
    if (remote_arg == NULL || remote_arg[0] == '\0') {
        return strlcpy(remote_path, name, remote_path_len) < remote_path_len;
    }

    const size_t remote_len = strlen(remote_arg);
    if (remote_arg[remote_len - 1] == '/') {
        return scp_append_path(remote_path, remote_path_len, remote_arg, name);
    }

    return strlcpy(remote_path, remote_arg, remote_path_len) < remote_path_len;
}

static void scp_render_usage(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = scp_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_write_bold(io, "scp");
    solar_os_shell_io_newline(io);
    solar_os_shell_io_writeln(io, "usage:");
    solar_os_shell_io_writeln(io, "  scp [-P port] local [user@]host:remote");
    solar_os_shell_io_writeln(io, "  scp [-P port] local [user@]host:");
    solar_os_shell_io_writeln(io, "  scp [-P port] [user@]host:remote local");
    solar_os_shell_io_writeln(io, "  scp [-P port] [user@]host:remote");
    solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
    solar_os_shell_io_flush(io);
}

static void scp_render_password_prompt(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = scp_io(ctx);

    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io,
                                  "scp %s %s@%s:%u\n",
                                  scp_app.direction == SOLAR_OS_SCP_UPLOAD ? "put" : "get",
                                  scp_app.username,
                                  scp_app.host,
                                  (unsigned)scp_app.port);
    solar_os_shell_io_write(io,
                            solar_os_ssh_keys_default_exists() ?
                                "password (Enter for key): " :
                                "password: ");
    for (size_t i = 0; i < scp_app.password_len; i++) {
        solar_os_shell_io_put_char(io, '*');
    }
    solar_os_shell_io_flush(io);
}

static esp_err_t scp_begin_transfer(solar_os_context_t *ctx)
{
    solar_os_shell_io_t *io = scp_io(ctx);

    scp_app.config = (solar_os_scp_config_t){
        .direction = scp_app.direction,
        .host = scp_app.host,
        .port = scp_app.port,
        .username = scp_app.username,
        .password = scp_app.password,
        .local_path = scp_app.local_path,
        .remote_path = scp_app.remote_path,
    };

    solar_os_shell_io_clear(io);
    solar_os_shell_io_printf_bold(io,
                                  "scp %s %s@%s:%u\n",
                                  scp_app.direction == SOLAR_OS_SCP_UPLOAD ? "put" : "get",
                                  scp_app.username,
                                  scp_app.host,
                                  (unsigned)scp_app.port);
    solar_os_shell_io_printf(io, "local:  %s\n", scp_app.local_path);
    solar_os_shell_io_printf(io, "remote: %s\n", scp_app.remote_path);
    solar_os_shell_io_writeln(io, "starting");
    solar_os_shell_io_flush(io);

    const esp_err_t err = solar_os_scp_start(&scp_app.config, &scp_app.session);
    memset(scp_app.password, 0, sizeof(scp_app.password));
    scp_app.password_len = 0;
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "scp start failed: %s\n", esp_err_to_name(err));
        solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
        solar_os_shell_io_flush(io);
        scp_app.mode = SCP_APP_ERROR;
        return err;
    }

    scp_app.mode = SCP_APP_RUNNING;
    return ESP_OK;
}

static void scp_print_progress(solar_os_shell_io_t *io, uint64_t transferred, uint64_t total)
{
    if (total > 0) {
        const int percent = (int)((transferred * 100ULL) / total);
        if (percent != 100 && percent < scp_app.last_percent + 10) {
            return;
        }
        scp_app.last_percent = percent;
        solar_os_shell_io_printf(io,
                                 "scp: %d%% %" PRIu64 "/%" PRIu64 "\n",
                                 percent,
                                 transferred,
                                 total);
        return;
    }

    if (transferred < scp_app.last_progress_step) {
        return;
    }
    scp_app.last_progress_step = transferred + 32768;
    solar_os_shell_io_printf(io, "scp: %" PRIu64 " bytes\n", transferred);
}

static void scp_drain_events(solar_os_context_t *ctx)
{
    if (scp_app.session == NULL) {
        return;
    }

    solar_os_shell_io_t *io = scp_io(ctx);
    solar_os_scp_event_t event;

    while (scp_app.session != NULL && solar_os_scp_poll(scp_app.session, &event)) {
        switch (event.type) {
        case SOLAR_OS_SCP_EVENT_STATUS:
            solar_os_shell_io_printf(io, "scp: %s\n", event.message);
            break;
        case SOLAR_OS_SCP_EVENT_PROGRESS:
            scp_print_progress(io, event.transferred, event.total);
            break;
        case SOLAR_OS_SCP_EVENT_ERROR:
            scp_app.saw_error = true;
            solar_os_shell_io_printf(io, "scp: %s\n", event.message);
            break;
        case SOLAR_OS_SCP_EVENT_DONE:
            solar_os_shell_io_printf(io, "scp: %s\n", event.message);
            solar_os_scp_stop(scp_app.session);
            scp_app.session = NULL;
            if (scp_app.saw_error) {
                scp_app.mode = SCP_APP_ERROR;
                solar_os_shell_io_printf(io, "%s exits\n", solar_os_shell_io_app_exit_key(io));
            } else {
                solar_os_context_request_exit(ctx);
            }
            break;
        default:
            break;
        }
    }
    solar_os_shell_io_flush(io);
}

static bool scp_parse_args(solar_os_context_t *ctx)
{
    int argc = solar_os_context_argc(ctx);
    int argi = 1;

    scp_app.port = SCP_DEFAULT_PORT;
    if (argc >= 4 && strcmp(solar_os_context_argv(ctx, argi), "-P") == 0) {
        if (!scp_parse_port(solar_os_context_argv(ctx, argi + 1), &scp_app.port)) {
            return false;
        }
        argi += 2;
    }

    const int operand_count = argc - argi;
    if (operand_count != 1 && operand_count != 2) {
        return false;
    }

    scp_target_t src;
    scp_target_t dst;
    if (!scp_parse_target(ctx, solar_os_context_argv(ctx, argi), &src)) {
        return false;
    }

    if (operand_count == 1) {
        if (!src.remote || src.path[0] == '\0') {
            return false;
        }

        scp_app.direction = SOLAR_OS_SCP_DOWNLOAD;
        strlcpy(scp_app.username, src.username, sizeof(scp_app.username));
        strlcpy(scp_app.host, src.host, sizeof(scp_app.host));
        strlcpy(scp_app.remote_path, src.path, sizeof(scp_app.remote_path));
        return scp_prepare_local_download_path(ctx,
                                               NULL,
                                               scp_app.remote_path,
                                               scp_app.local_path,
                                               sizeof(scp_app.local_path));
    }

    if (!scp_parse_target(ctx, solar_os_context_argv(ctx, argi + 1), &dst)) {
        return false;
    }
    if (src.remote == dst.remote) {
        return false;
    }

    if (src.remote) {
        if (src.path[0] == '\0') {
            return false;
        }
        scp_app.direction = SOLAR_OS_SCP_DOWNLOAD;
        strlcpy(scp_app.username, src.username, sizeof(scp_app.username));
        strlcpy(scp_app.host, src.host, sizeof(scp_app.host));
        strlcpy(scp_app.remote_path, src.path, sizeof(scp_app.remote_path));
        return scp_prepare_local_download_path(ctx,
                                               solar_os_context_argv(ctx, argi + 1),
                                               scp_app.remote_path,
                                               scp_app.local_path,
                                               sizeof(scp_app.local_path));
    }

    scp_app.direction = SOLAR_OS_SCP_UPLOAD;
    strlcpy(scp_app.username, dst.username, sizeof(scp_app.username));
    strlcpy(scp_app.host, dst.host, sizeof(scp_app.host));
    strlcpy(scp_app.local_path, src.path, sizeof(scp_app.local_path));
    return scp_prepare_remote_upload_path(dst.path,
                                          scp_app.local_path,
                                          scp_app.remote_path,
                                          sizeof(scp_app.remote_path));
}

static esp_err_t scp_start(solar_os_context_t *ctx)
{
    memset(&scp_app, 0, sizeof(scp_app));
    scp_app.last_percent = -10;

    if (!scp_parse_args(ctx)) {
        scp_app.mode = SCP_APP_ERROR;
        scp_render_usage(ctx);
        return ESP_OK;
    }

    scp_app.mode = SCP_APP_PASSWORD;
    scp_render_password_prompt(ctx);
    return ESP_OK;
}

static void scp_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (scp_app.session != NULL) {
        solar_os_scp_stop(scp_app.session);
    }
    memset(&scp_app, 0, sizeof(scp_app));
}

static bool scp_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (event->type == SOLAR_OS_EVENT_TICK) {
        scp_drain_events(ctx);
        return true;
    }
    if (event->type != SOLAR_OS_EVENT_CHAR) {
        return false;
    }

    const char ch = event->data.ch;
    if ((uint8_t)ch == SOLAR_OS_KEY_APP_EXIT) {
        if (scp_app.session != NULL) {
            solar_os_shell_io_t *io = scp_io(ctx);
            solar_os_shell_io_writeln(io, "\nscp: cancelling");
            solar_os_shell_io_flush(io);
            solar_os_scp_stop(scp_app.session);
            scp_app.session = NULL;
        }
        solar_os_context_request_exit(ctx);
        return true;
    }

    switch (scp_app.mode) {
    case SCP_APP_PASSWORD:
        if (ch == '\b') {
            if (scp_app.password_len > 0) {
                scp_app.password_len--;
                scp_app.password[scp_app.password_len] = '\0';
                scp_render_password_prompt(ctx);
            }
        } else if (ch == '\r' || ch == '\n') {
            (void)scp_begin_transfer(ctx);
        } else if (scp_is_printable(ch) && scp_app.password_len + 1 < sizeof(scp_app.password)) {
            scp_app.password[scp_app.password_len++] = ch;
            scp_app.password[scp_app.password_len] = '\0';
            scp_render_password_prompt(ctx);
        }
        break;
    case SCP_APP_RUNNING:
        scp_drain_events(ctx);
        break;
    case SCP_APP_ERROR:
    default:
        break;
    }

    return true;
}

const solar_os_app_t solar_os_scp_app = {
    .name = "scp",
    .summary = "SCP file copy",
    .start = scp_start,
    .stop = scp_stop,
    .event = scp_event,
};
